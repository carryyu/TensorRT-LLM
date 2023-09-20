# SPDX-FileCopyrightText: Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
import argparse
import json
import os
import time

import tensorrt as trt
import torch
import torch.multiprocessing as mp
from safetensors import safe_open
from transformers import AutoModelForCausalLM, GPTNeoXConfig
from weight import load_from_hf_gpt_neox

import tensorrt_llm
from tensorrt_llm.builder import Builder
from tensorrt_llm.logger import logger
from tensorrt_llm.mapping import Mapping
from tensorrt_llm.models import (weight_only_groupwise_quantize,
                                 weight_only_quantize)
from tensorrt_llm.network import net_guard
from tensorrt_llm.plugin.plugin import ContextFMHAType
from tensorrt_llm.quantization import QuantMode

MODEL_NAME = "gptneox"
hf_gpt = None


class StateDict():

    def __init__(self, model_dir):
        self.model_state_dict = safe_open(model_dir +
                                          '/gptneox-20b-4bit-gs128.safetensors',
                                          framework="pt",
                                          device=0)

    def get(self, k):
        return self.model_state_dict.get_tensor(k).cpu()


class GPTQModel():

    def __init__(self, model_dir):
        with open(model_dir + '/config.json', 'r') as f:
            model_config = json.load(f)
            self.config = GPTNeoXConfig()
            self.config.vocab_size = model_config['vocab_size']
            self.config.hidden_size = model_config['hidden_size']
            self.config.num_hidden_layers = model_config['num_hidden_layers']
            self.config.num_attention_heads = model_config[
                'num_attention_heads']
            self.config.intermediate_size = model_config['intermediate_size']
            self.config.hidden_act = model_config['hidden_act']
            self.config.rotary_pct = model_config['rotary_pct']
            self.config.rotary_emb_base = model_config['rotary_emb_base']
            self.config.max_position_embeddings = model_config[
                'max_position_embeddings']
            self.config.initializer_range = model_config['initializer_range']
            self.config.layer_norm_eps = model_config['layer_norm_eps']
            self.config.use_cache = model_config['use_cache']
            self.config.bos_token_id = model_config['bos_token_id']
            self.config.eos_token_id = model_config['eos_token_id']
            self.config.tie_word_embeddings = model_config[
                'tie_word_embeddings']
        self.model_state_dict = StateDict(model_dir)

    def state_dict(self):
        return self.model_state_dict


def get_engine_name(model, dtype, tp_size, rank):
    return '{}_{}_tp{}_rank{}.engine'.format(model, dtype, tp_size, rank)


def serialize_engine(engine, path):
    logger.info(f'Serializing engine to {path}...')
    tik = time.time()
    with open(path, 'wb') as f:
        f.write(bytearray(engine))
    tok = time.time()
    t = time.strftime('%H:%M:%S', time.gmtime(tok - tik))
    logger.info(f'Engine serialized. Total time: {t}')


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument('--world_size',
                        type=int,
                        default=1,
                        help='world size, only support tensor parallelism now')
    parser.add_argument(
        '--model_dir',
        type=str,
        default=None,
        help='The path to HF GPT-NeoX model / checkpoints to read weights from')
    parser.add_argument('--dtype',
                        type=str,
                        default='float16',
                        choices=['float16', 'float32'])
    parser.add_argument(
        '--timing_cache',
        type=str,
        default='model.cache',
        help=
        'The path of to read timing cache from, will be ignored if the file does not exist'
    )
    parser.add_argument('--log_level', type=str, default='info')
    parser.add_argument('--vocab_size', type=int, default=50432)
    parser.add_argument('--n_layer', type=int, default=44)
    parser.add_argument('--n_positions', type=int, default=2048)
    parser.add_argument('--n_embd', type=int, default=6144)
    parser.add_argument('--n_head', type=int, default=64)
    parser.add_argument('--hidden_act', type=str, default='gelu')
    parser.add_argument(
        '--rotary_pct',
        type=float,
        default=0.25,
        help="Percentage of hidden dimensions to allocate to rotary embeddings."
    )
    parser.add_argument('--max_batch_size', type=int, default=64)
    parser.add_argument('--max_input_len', type=int, default=1024)
    parser.add_argument('--max_output_len', type=int, default=1024)
    parser.add_argument('--max_beam_width', type=int, default=1)
    parser.add_argument('--use_gpt_attention_plugin',
                        nargs='?',
                        const='float16',
                        type=str,
                        default=False,
                        choices=['float16', 'float32'])
    parser.add_argument('--use_gemm_plugin',
                        nargs='?',
                        const='float16',
                        type=str,
                        default=False,
                        choices=['float16', 'float32'])
    parser.add_argument('--use_weight_only_quant_matmul_plugin',
                        nargs='?',
                        const='float16',
                        type=str,
                        default=False,
                        choices=['float16'])
    parser.add_argument('--use_weight_only_groupwise_quant_matmul_plugin',
                        nargs='?',
                        const='float16',
                        type=str,
                        default=False,
                        choices=['float16'])
    parser.add_argument('--use_layernorm_plugin',
                        nargs='?',
                        const='float16',
                        type=str,
                        default=False,
                        choices=['float16', 'float32'])
    parser.add_argument('--parallel_build', default=False, action='store_true')
    parser.add_argument('--enable_context_fmha',
                        default=False,
                        action='store_true')
    parser.add_argument('--enable_context_fmha_fp32_acc',
                        default=False,
                        action='store_true')
    parser.add_argument('--gpus_per_node', type=int, default=8)
    parser.add_argument(
        '--output_dir',
        type=str,
        default='gpt_outputs',
        help=
        'The path to save the serialized engine files, timing cache file and model configs'
    )
    parser.add_argument('--remove_input_padding',
                        default=False,
                        action='store_true')

    args = parser.parse_args()

    logger.set_level(args.log_level)

    if args.model_dir is not None:
        global hf_gpt
        if not args.use_weight_only_groupwise_quant_matmul_plugin:
            logger.info(f'Loading HF GPT-NeoX model from {args.model_dir}...')
            hf_gpt = AutoModelForCausalLM.from_pretrained(args.model_dir)
            args.n_embd = hf_gpt.config.hidden_size
            args.n_head = hf_gpt.config.num_attention_heads
            args.n_layer = hf_gpt.config.num_hidden_layers
            args.n_positions = hf_gpt.config.max_position_embeddings
            args.vocab_size = hf_gpt.config.vocab_size
            args.rotary_pct = hf_gpt.config.rotary_pct
        else:
            logger.info(
                f'Loading GPTQ quantized HF GPT-NeoX model from {args.model_dir}...'
            )
            hf_gpt = GPTQModel(args.model_dir)
            args.n_embd = hf_gpt.config.hidden_size
            args.n_head = hf_gpt.config.num_attention_heads
            args.n_layer = hf_gpt.config.num_hidden_layers
            args.n_positions = hf_gpt.config.max_position_embeddings
            args.vocab_size = hf_gpt.config.vocab_size
            args.rotary_pct = hf_gpt.config.rotary_pct

    return args


def build_rank_engine(builder: Builder,
                      builder_config: tensorrt_llm.builder.BuilderConfig,
                      engine_name, rank, args):
    '''
       @brief: Build the engine on the given rank.
       @param rank: The rank to build the engine.
       @param args: The cmd line arguments.
       @return: The built engine.
    '''
    kv_dtype = trt.float16 if args.dtype == 'float16' else trt.float32
    rotary_dim = int((args.n_embd // args.n_head) * args.rotary_pct)

    # Initialize Module
    tensorrt_llm_gpt = tensorrt_llm.models.GPTNeoXForCausalLM(
        num_layers=args.n_layer,
        num_heads=args.n_head,
        hidden_size=args.n_embd,
        vocab_size=args.vocab_size,
        hidden_act=args.hidden_act,
        max_position_embeddings=args.n_positions,
        rotary_dim=rotary_dim,
        dtype=kv_dtype,
        mapping=Mapping(world_size=args.world_size,
                        rank=rank,
                        tp_size=args.world_size),  # TP only
        apply_query_key_layer_scaling=builder_config.
        apply_query_key_layer_scaling)
    if args.use_weight_only_quant_matmul_plugin:
        tensorrt_llm_gpt = weight_only_quantize(tensorrt_llm_gpt)

    if args.use_weight_only_groupwise_quant_matmul_plugin:
        tensorrt_llm_gpt = weight_only_groupwise_quantize(
            model=tensorrt_llm_gpt,
            quant_mode=QuantMode(0),
            group_size=128,
            zero=True)

    if args.model_dir is not None:
        assert hf_gpt is not None, f'Could not load weights from hf_gpt model as it is not loaded yet.'

        if args.world_size > 1:
            assert (
                args.n_embd % args.world_size == 0
            ), f'Embedding size/hidden size must be divisible by world size.'
            assert (
                args.n_head % args.world_size == 0
            ), f'Number of attention heads must be divisible by world size.'

        load_from_hf_gpt_neox(
            tensorrt_llm_gpt, hf_gpt, (args.dtype == 'float16'), rank,
            args.world_size, args.use_weight_only_groupwise_quant_matmul_plugin)

    # Module -> Network
    network = builder.create_network()
    network.trt_network.name = engine_name
    if args.use_gpt_attention_plugin:
        network.plugin_config.set_gpt_attention_plugin(
            dtype=args.use_gpt_attention_plugin)
    if args.use_gemm_plugin:
        network.plugin_config.set_gemm_plugin(dtype=args.use_gemm_plugin)
    if args.use_layernorm_plugin:
        network.plugin_config.set_layernorm_plugin(
            dtype=args.use_layernorm_plugin)
    assert not (args.enable_context_fmha and args.enable_context_fmha_fp32_acc)
    if args.enable_context_fmha:
        network.plugin_config.set_context_fmha(ContextFMHAType.enabled)
    if args.enable_context_fmha_fp32_acc:
        network.plugin_config.set_context_fmha(
            ContextFMHAType.enabled_with_fp32_acc)
    if args.use_weight_only_quant_matmul_plugin:
        network.plugin_config.set_weight_only_quant_matmul_plugin(
            dtype=args.use_weight_only_quant_matmul_plugin)
    if args.use_weight_only_groupwise_quant_matmul_plugin:
        network.plugin_config.set_weight_only_groupwise_quant_matmul_plugin(
            dtype=args.use_weight_only_groupwise_quant_matmul_plugin)
    if args.world_size > 1:
        network.plugin_config.set_nccl_plugin(args.dtype)
    if args.remove_input_padding:
        network.plugin_config.enable_remove_input_padding()
    with net_guard(network):
        # Prepare
        network.set_named_parameters(tensorrt_llm_gpt.named_parameters())

        # Forward
        inputs = tensorrt_llm_gpt.prepare_inputs(args.max_batch_size,
                                                 args.max_input_len,
                                                 args.max_output_len, True,
                                                 args.max_beam_width)
        tensorrt_llm_gpt(*inputs)

    engine = None

    # Network -> Engine
    engine = builder.build_engine(network, builder_config)
    if rank == 0:
        config_path = os.path.join(args.output_dir, 'config.json')
        builder.save_config(builder_config, config_path)
    return engine


def build(rank, args):
    torch.cuda.set_device(rank % args.gpus_per_node)
    tensorrt_llm.logger.set_level(args.log_level)
    if not os.path.exists(args.output_dir):
        os.makedirs(args.output_dir)

    # when doing serializing build, all ranks share one engine
    apply_query_key_layer_scaling = False
    builder = Builder()

    cache = None
    for cur_rank in range(args.world_size):
        # skip other ranks if parallel_build is enabled
        if args.parallel_build and cur_rank != rank:
            continue
        builder_config = builder.create_builder_config(
            name=MODEL_NAME,
            precision=args.dtype,
            timing_cache=args.timing_cache if cache is None else cache,
            tensor_parallel=args.world_size,  # TP only
            parallel_build=args.parallel_build,
            num_layers=args.n_layer,
            num_heads=args.n_head,
            hidden_size=args.n_embd,
            vocab_size=args.vocab_size,
            hidden_act=args.hidden_act,
            max_position_embeddings=args.n_positions,
            apply_query_key_layer_scaling=apply_query_key_layer_scaling,
            max_batch_size=args.max_batch_size,
            max_input_len=args.max_input_len,
            max_output_len=args.max_output_len)

        engine_name = get_engine_name(MODEL_NAME, args.dtype, args.world_size,
                                      cur_rank)
        engine = build_rank_engine(builder, builder_config, engine_name,
                                   cur_rank, args)
        assert engine is not None, f'Failed to build engine for rank {cur_rank}'

        if cur_rank == 0:
            # Use in-memory timing cache for multiple builder passes.
            if not args.parallel_build:
                cache = builder_config.trt_builder_config.get_timing_cache()

        serialize_engine(engine, os.path.join(args.output_dir, engine_name))

    if rank == 0:
        ok = builder.save_timing_cache(
            builder_config, os.path.join(args.output_dir, "model.cache"))
        assert ok, "Failed to save timing cache."


if __name__ == '__main__':
    args = parse_arguments()
    tik = time.time()
    if args.parallel_build and args.world_size > 1 and \
            torch.cuda.device_count() >= args.world_size:
        logger.warning(
            f'Parallelly build TensorRT engines. Please make sure that all of the {args.world_size} GPUs are totally free.'
        )
        mp.spawn(build, nprocs=args.world_size, args=(args, ))
    else:
        args.parallel_build = False
        logger.info('Serially build TensorRT engines.')
        build(0, args)

    tok = time.time()
    t = time.strftime('%H:%M:%S', time.gmtime(tok - tik))
    logger.info(f'Total time of building all {args.world_size} engines: {t}')
