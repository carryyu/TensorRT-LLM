/*
 * Copyright (c) 2022-2023, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include "tensorrt_llm/common/memoryUtils.h"
#include "tensorrt_llm/runtime/bufferManager.h"
#include "tensorrt_llm/runtime/common.h"
#include "tensorrt_llm/runtime/cudaStream.h"
#include "tensorrt_llm/runtime/gptJsonConfig.h"
#include "tensorrt_llm/runtime/gptModelConfig.h"
#include "tensorrt_llm/runtime/iBuffer.h"
#include "tensorrt_llm/runtime/iTensor.h"
#include "tensorrt_llm/runtime/loraCache.h"
#include "tensorrt_llm/runtime/loraManager.h"
#include "tensorrt_llm/runtime/loraModule.h"
#include "tensorrt_llm/runtime/loraUtils.h"
#include "tensorrt_llm/runtime/worldConfig.h"

#include "tensorrt_llm/runtime/utils/numpyUtils.h"

#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
auto const TEST_RESOURCE_PATH = fs::path{TOP_LEVEL_DIR} / "cpp/tests/resources/data";
auto const TEST_SOURCE_LORA_TP1 = TEST_RESOURCE_PATH / "lora-test-weights-tp1/source.npy";
auto const TEST_DEST_LORA_TP1 = TEST_RESOURCE_PATH / "lora-test-weights-tp1/target.npy";
auto const TEST_KEYS_LORA_TP1 = TEST_RESOURCE_PATH / "lora-test-weights-tp1/config.npy";
auto const TEST_SOURCE_LORA_TP2 = TEST_RESOURCE_PATH / "lora-test-weights-tp2/source.npy";
auto const TEST_DEST_LORA_TP2 = TEST_RESOURCE_PATH / "lora-test-weights-tp2/target.npy";
auto const TEST_KEYS_LORA_TP2 = TEST_RESOURCE_PATH / "lora-test-weights-tp2/config.npy";
auto const TEST_MODEL_CONFIG = TEST_RESOURCE_PATH / "test_model_lora_config.json";
} // namespace

namespace tensorrt_llm::runtime
{
using TensorPtr = ITensor::SharedPtr;
using PeftTable = LoraManager::PeftTable;

class LoraManagerTest : public ::testing::Test // NOLINT(cppcoreguidelines-pro-type-member-init)
{
protected:
    LoraManagerTest()
        : mModelConfig(1, 2, 1, 4, nvinfer1::DataType::kFLOAT)
    {
    }

    void SetUp() override
    {
        mStream = std::make_unique<CudaStream>();
        mManager = std::make_unique<BufferManager>(mStream);

        mWorldConfig = WorldConfig(2);

        mModelConfig.setLoraModules(LoraModule::createLoraModules({"attn_dense", "attn_qkv"}, 4, 4, 1, 1, 2, 2));
    }

    std::unique_ptr<BufferManager> mManager;
    BufferManager::CudaStreamPtr mStream;
    GptModelConfig mModelConfig;
    WorldConfig mWorldConfig;

    PeftTable getPeftTable(SizeType tpRank = 0)
    {
        auto modelConfig = GptModelConfig(0, 2, 1, 16, nvinfer1::DataType::kFLOAT);
        modelConfig.setMlpHiddenSize(32);
        auto worldConfig = WorldConfig(2, 2, 3);
        std::vector<LoraModule> modules{
            LoraModule(LoraModule::ModuleType::kATTN_QKV, 16, 3 * 16, false, true, -1, 0),
            LoraModule(LoraModule::ModuleType::kATTN_Q, 16, 16, false, true, -1, 0),
            LoraModule(LoraModule::ModuleType::kATTN_K, 16, 16, false, true, -1, 0),
            LoraModule(LoraModule::ModuleType::kATTN_V, 16, 16, false, true, -1, 0),
            LoraModule(LoraModule::ModuleType::kATTN_DENSE, 16, 16, false, true, 1, -1),
            LoraModule(LoraModule::ModuleType::kMLP_H_TO_4H, 16, 32, false, true, -1, 0),
            LoraModule(LoraModule::ModuleType::kMLP_4H_TO_H, 32, 16, false, true, 1, -1),
            LoraModule(LoraModule::ModuleType::kMLP_GATE, 16, 32, false, true, -1, 0),
        };
        modelConfig.setLoraModules(modules);
        auto pageConfig = LoraCachePageManagerConfig(
            runtime::MemoryType::kCPU, nvinfer1::DataType::kFLOAT, 2 * 8, 6, 64, 4 * 16, 1);
        pageConfig.setInitToZero(true);
        LoraCache loraCache(pageConfig, modelConfig, worldConfig, *mManager);

        TensorPtr loraReqWeights = utils::loadNpy(*mManager, TEST_SOURCE_LORA_TP2, MemoryType::kCPU);
        TensorPtr loraReqKeys = utils::loadNpy(*mManager, TEST_KEYS_LORA_TP2, MemoryType::kCPU);

        loraCache.put(1234, loraReqWeights, loraReqKeys);
        PeftTable peftTable{};
        peftTable.try_emplace(1234, loraCache.get(1234));
        return peftTable;
    }
};

TEST_F(LoraManagerTest, moduleParsing)
{
    auto jsonConfig = GptJsonConfig::parse(TEST_MODEL_CONFIG);
    auto loraModules = jsonConfig.getModelConfig().getLoraModules();

    std::vector<LoraModule> expectedModules{
        LoraModule(LoraModule::ModuleType::kATTN_QKV, 2048, 6144, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kATTN_Q, 2048, 2048, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kATTN_K, 2048, 2048, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kATTN_V, 2048, 2048, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kATTN_DENSE, 2048, 2048, false, true, 1, -1),
        LoraModule(LoraModule::ModuleType::kMLP_GATE, 2048, 4096, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kMLP_H_TO_4H, 2048, 4096, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kMLP_4H_TO_H, 4096, 2048, false, true, 1, -1),
        LoraModule(LoraModule::ModuleType::kCROSS_ATTN_QKV, 2048, 6144, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kCROSS_ATTN_Q, 2048, 2048, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kCROSS_ATTN_K, 2048, 2048, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kCROSS_ATTN_V, 2048, 2048, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kCROSS_ATTN_DENSE, 2048, 2048, false, true, 1, -1),
    };
    ASSERT_EQ(expectedModules.size(), loraModules.size());
    for (size_t i = 0; i < expectedModules.size(); ++i)
    {
        EXPECT_EQ(expectedModules[i].value(), loraModules[i].value());
        EXPECT_EQ(expectedModules[i].name(), loraModules[i].name());
        EXPECT_EQ(expectedModules[i].inDim(), loraModules[i].inDim());
        EXPECT_EQ(expectedModules[i].outDim(), loraModules[i].outDim());
        EXPECT_EQ(expectedModules[i].inTpSplitDim(), loraModules[i].inTpSplitDim());
        EXPECT_EQ(expectedModules[i].outTpSplitDim(), loraModules[i].outTpSplitDim());
    }
}

static void checkLoraTensors(LoraManager const& loraManager, std::vector<int64_t> const& targetPtrs,
    TensorPtr weightsPtrs, std::vector<int32_t> const& targetAdapterSizes, TensorPtr adapterSizes,
    GptModelConfig const& modelConfig, WorldConfig const& worldConfig, std::vector<LoraModule> const& modules,
    SizeType numModules, SizeType numLayers, SizeType numSeqs, bool checkPointers = true)
{
    auto adapterSizesPtr = bufferCast<SizeType>(*adapterSizes);
    auto weightsPtrsPtr = bufferCast<int64_t>(*weightsPtrs);
    ASSERT_EQ(targetPtrs.size(), weightsPtrs->getSize());
    ASSERT_EQ(targetAdapterSizes.size(), adapterSizes->getSize());
    auto firstLayerId
        = modelConfig.getNbLayers(worldConfig.getPipelineParallelism()) * worldConfig.getPipelineParallelRank();
    LoraManager::TensorMap expectedTensors;

    for (SizeType m = 0; m < numModules; ++m)
    {
        TensorPtr modSlice = ITensor::slice(weightsPtrs, m, 1);
        TensorPtr modAdapterSlice = ITensor::slice(adapterSizes, m, 1);
        modSlice->squeeze(0);
        modAdapterSlice->squeeze(0);
        for (SizeType l = 0; l < numLayers; ++l)
        {
            TensorPtr layerSlice = ITensor::slice(modSlice, l, 1);
            TensorPtr layerAdapterSlice = ITensor::slice(modAdapterSlice, l, 1);
            layerSlice->squeeze(0);
            layerAdapterSlice->squeeze(0);
            auto weightsFieldName
                = std::string(modules.at(m).name()) + "_lora_weights_pointers_" + std::to_string(l + firstLayerId);
            expectedTensors.insert_or_assign(weightsFieldName, layerSlice);
            auto adapterSizeFieldName
                = std::string(modules.at(m).name()) + "_lora_ranks_" + std::to_string(l + firstLayerId);
            expectedTensors.insert_or_assign(adapterSizeFieldName, layerAdapterSlice);
            for (SizeType i = 0; i < numSeqs; ++i)
            {
                SizeType adapterSizeOff = common::flat_index3(m, l, i, numLayers, numSeqs);
                EXPECT_EQ(targetAdapterSizes[adapterSizeOff], adapterSizesPtr[adapterSizeOff]);
                SizeType inPtrIdx = common::flat_index4(m, l, i, 0, numLayers, numSeqs, 2);
                SizeType outPtrIdx = common::flat_index4(m, l, i, 1, numLayers, numSeqs, 2);
                std::cout << weightsPtrsPtr[inPtrIdx] << " " << weightsPtrsPtr[outPtrIdx] << std::endl;
                if (checkPointers || targetPtrs[inPtrIdx] == 0)
                {
                    EXPECT_EQ(targetPtrs[inPtrIdx], weightsPtrsPtr[inPtrIdx]);
                    EXPECT_EQ(targetPtrs[outPtrIdx], weightsPtrsPtr[outPtrIdx]);
                }
                else
                {
                    EXPECT_NE(0, weightsPtrsPtr[inPtrIdx]);
                    EXPECT_NE(0, weightsPtrsPtr[outPtrIdx]);
                }
            }
        }
    }

    LoraManager::TensorMap inputTensors;
    loraManager.insertInputTensors(inputTensors, weightsPtrs, adapterSizes, modelConfig, worldConfig);

    ASSERT_EQ(expectedTensors.size(), inputTensors.size());
    for (auto& [fieldName, tensor] : expectedTensors)
    {
        ASSERT_NE(inputTensors.find(fieldName), inputTensors.end());
        auto expectedTensor = expectedTensors.find(fieldName)->second;
        auto actualTensor = inputTensors.find(fieldName)->second;
        ITensor::shapeEquals(expectedTensor->getShape(), actualTensor->getShape());
        if (expectedTensor->getDataType() == nvinfer1::DataType::kINT64)
        {
            auto expT = bufferCast<int64_t>(*expectedTensor);
            auto actT = bufferCast<int64_t>(*actualTensor);
            for (size_t i = 0; i < expectedTensor->getSize(); ++i)
            {
                EXPECT_EQ(expT[i], actT[i]);
            }
        }
        else
        {
            auto expT = bufferCast<int32_t>(*expectedTensor);
            auto actT = bufferCast<int32_t>(*actualTensor);
            for (size_t i = 0; i < expectedTensor->getSize(); ++i)
            {
                EXPECT_EQ(expT[i], actT[i]);
            }
        }
    }
}

static std::tuple<std::vector<int32_t>, std::vector<int64_t>, PeftTable> createFillInputTensorsTestsData(
    std::vector<TensorPtr> const& configs, std::vector<uint64_t> const& reqIds,
    std::vector<SizeType> const& reqBeamWidth, std::vector<LoraModule> const& modules, SizeType numLayers,
    SizeType numSeq, std::vector<LoraCache::TaskLayerModuleConfigListPtr>& valuesWorkspace)
{
    std::map<SizeType, SizeType> moduleOffset;
    SizeType modOff = 0;
    for (auto const& m : modules)
    {
        moduleOffset[m.value()] = modOff++;
    }

    SizeType batchSize = configs.size();
    SizeType numModules = modules.size();

    std::vector<int32_t> targetAdapterSizes(numModules * numLayers * numSeq, 0);
    std::vector<int64_t> targetPointers(numModules * numLayers * numSeq * 2, 0);

    PeftTable peftTable{};

    int64_t pointerAddr = 777001;

    for (size_t bid = 0; bid < configs.size(); ++bid)
    {
        valuesWorkspace.push_back(std::make_shared<std::vector<LoraCache::TaskLayerModuleConfig>>());
        auto beamWidth = reqBeamWidth[bid];
        auto config = configs[bid];
        if (config == nullptr)
        {
            continue;
        }
        peftTable.try_emplace(reqIds[bid], valuesWorkspace[bid]);
        if (config->getShape().nbDims == 3)
        {
            config->squeeze(0);
        }
        SizeType numRows = config->getShape().d[0];
        for (SizeType r = 0; r < numRows; ++r)
        {
            auto const* row = bufferCast<int32_t>(*ITensor::slice(config, r, 1));
            auto moduleId = row[lora::kLORA_CONFIG_MODULE_OFF];
            auto layerId = row[lora::kLORA_CONFIG_LAYER_OFF];
            auto adapterSize = row[lora::kLORA_CONFIG_ADAPTER_SIZE_OFF];
            auto modOff = moduleOffset.at(moduleId);

            auto inPointer = pointerAddr++;
            auto outPointer = pointerAddr++;
            valuesWorkspace[bid]->push_back(
                LoraCache::TaskLayerModuleConfig{0, 0, 0, 0, moduleId, layerId, adapterSize, 0, inPointer, outPointer});

            for (SizeType beamIdx = 0; beamIdx < beamWidth; ++beamIdx)
            {
                targetAdapterSizes[common::flat_index3<size_t>(modOff, layerId, bid + beamIdx, numLayers, numSeq)]
                    = adapterSize;
                targetPointers[common::flat_index4<size_t>(modOff, layerId, bid + beamIdx, 0, numLayers, numSeq, 2)]
                    = inPointer;
                targetPointers[common::flat_index4<size_t>(modOff, layerId, bid + beamIdx, 1, numLayers, numSeq, 2)]
                    = outPointer;
            }
        }
    }

    return std::make_tuple(targetAdapterSizes, targetPointers, peftTable);
}

TEST_F(LoraManagerTest, fillInputTensors)
{
    LoraManager loraManager;
    auto modelConfig = GptModelConfig(0, 2, 1, 16, nvinfer1::DataType::kFLOAT);
    modelConfig.setMlpHiddenSize(32);
    auto worldConfig = WorldConfig(1, 1, 0);
    std::vector<LoraModule> modules{
        LoraModule(LoraModule::ModuleType::kATTN_QKV, 16, 3 * 16, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kATTN_Q, 16, 16, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kATTN_K, 16, 16, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kATTN_V, 16, 16, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kATTN_DENSE, 16, 16, false, true, 1, -1),
        LoraModule(LoraModule::ModuleType::kMLP_H_TO_4H, 16, 32, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kMLP_GATE, 16, 32, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kMLP_4H_TO_H, 32, 16, false, true, 1, -1),
        LoraModule(LoraModule::ModuleType::kCROSS_ATTN_QKV, 16, 3 * 16, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kCROSS_ATTN_Q, 16, 16, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kCROSS_ATTN_K, 16, 16, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kCROSS_ATTN_V, 16, 16, false, true, -1, 0),
        LoraModule(LoraModule::ModuleType::kCROSS_ATTN_DENSE, 16, 16, false, true, 1, -1),
    };
    modelConfig.setLoraModules(modules);
    loraManager.create(modelConfig, worldConfig, *mManager);
    auto numModules = static_cast<SizeType>(modelConfig.getLoraModules().size());
    auto numLayers = static_cast<SizeType>(modelConfig.getNbLayers());
    SizeType numSeqs = 4;
    TensorPtr weightsPtrs
        = mManager->cpu(ITensor::makeShape({numModules, numLayers, numSeqs, 2}), nvinfer1::DataType::kINT64);
    TensorPtr adapterSizes
        = mManager->cpu(ITensor::makeShape({numModules, numLayers, numSeqs}), nvinfer1::DataType::kINT32);

    mManager->setZero(*weightsPtrs);
    mManager->setZero(*adapterSizes);

    SizeType numContextRequests = 1;
    std::vector<uint64_t> reqIds{1, 2, 3};
    std::vector<SizeType> reqBeamWidth{1, 2, 1};

    TensorPtr loraReqKeys = utils::loadNpy(*mManager, TEST_KEYS_LORA_TP1, MemoryType::kCPU);
    std::vector<TensorPtr> loraConfigs{loraReqKeys, loraReqKeys, nullptr};

    std::vector<LoraCache::TaskLayerModuleConfigListPtr> valuesWorkspace;
    auto [targetadapterSizes, targetPointers, peftTable] = createFillInputTensorsTestsData(
        loraConfigs, reqIds, reqBeamWidth, modules, numLayers, numSeqs, valuesWorkspace);

    loraManager.fillInputTensors(weightsPtrs, adapterSizes, peftTable, reqIds, reqBeamWidth, modelConfig, worldConfig);

    auto adapterSizesPtr = bufferCast<SizeType>(*adapterSizes);
    auto weightsPtrsPtr = bufferCast<int64_t>(*weightsPtrs);

    checkLoraTensors(loraManager, targetPointers, weightsPtrs, targetadapterSizes, adapterSizes, modelConfig,
        worldConfig, modules, numModules, numLayers, numSeqs);
}
} // namespace tensorrt_llm::runtime
