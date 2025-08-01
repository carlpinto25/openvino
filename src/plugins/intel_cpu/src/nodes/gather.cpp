// Copyright (C) 2018-2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "gather.h"

#include <partitioned_mem_blk.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <numeric>
#include <oneapi/dnnl/dnnl_common.hpp>
#include <openvino/op/constant.hpp>
#include <openvino/op/gather.hpp>
#include <string>
#include <vector>

#include "common/cpu_memcpy.h"
#include "cpu_memory.h"
#include "cpu_types.h"
#include "edge.h"
#include "graph_context.h"
#include "memory_desc/cpu_memory_desc.h"
#include "node.h"
#include "nodes/common/cpu_convert.h"
#include "onednn/iml_type_mapper.h"
#include "openvino/cc/selective_build.h"
#include "openvino/core/except.hpp"
#include "openvino/core/node.hpp"
#include "openvino/core/parallel.hpp"
#include "openvino/core/type.hpp"
#include "openvino/core/type/bfloat16.hpp"
#include "openvino/core/type/element_type.hpp"
#include "openvino/core/type/float16.hpp"
#include "ov_ops/gather_compressed.hpp"
#include "selective_build.h"
#include "shape_inference/custom/gather.hpp"
#include "utils/debug_capabilities.h"
#include "utils/general_utils.h"
#include "utils/ngraph_utils.hpp"

#if defined(OPENVINO_ARCH_X86) || defined(OPENVINO_ARCH_X86_64)
#    include <cpu/x64/cpu_isa_traits.hpp>

#    include "kernels/x64/gather_uni_kernel.hpp"
#endif

using namespace dnnl::impl::cpu;

namespace ov::intel_cpu::node {

bool Gather::isSupportedOperation(const std::shared_ptr<const ov::Node>& op, std::string& errorMessage) noexcept {
    try {
        const auto gather_compression = ov::as_type_ptr<const ov::op::internal::GatherCompressed>(op);
        if (gather_compression) {
            return true;
        }

        if (op->get_output_element_type(0) == element::string) {
            return false;
        }
        if (none_of(op->get_type_info(),
                    ov::op::v7::Gather::get_type_info_static(),
                    ov::op::v8::Gather::get_type_info_static())) {
            errorMessage = "Not supported Gather operation version. CPU plug-in supports only 7 and 8 versions.";
            return false;
        }

        if (!isDynamicNgraphNode(op) && !ov::is_type<ov::op::v0::Constant>(op->get_input_node_ptr(GATHER_AXIS))) {
            errorMessage = "Only Constant operation on 'axis' input is supported for static node.";
            return false;
        }
    } catch (...) {
        return false;
    }

    return true;
}

Gather::Gather(const std::shared_ptr<ov::Node>& op, const GraphContext::CPtr& context)
    : Node(op, context, GatherShapeInferFactory(op)) {
    std::string errorMessage;
    if (!isSupportedOperation(op, errorMessage)) {
        OPENVINO_THROW_NOT_IMPLEMENTED(errorMessage);
    }

    if (any_of(op->get_input_size(), 4U, 5U) && op->get_output_size() == 1U) {
        compressed = true;
    } else {
        CPU_NODE_ASSERT(op->get_input_size() == 3 && op->get_output_size() == 1,
                        "has incorrect number of input/output edges!");
    }

    const auto& dataShape = getInputShapeAtPort(GATHER_DATA);
    isDataShapeStat = dataShape.isStatic();
    dataSrcRank = dataShape.getRank();

    const auto& idxShape = getInputShapeAtPort(GATHER_INDICES);
    isIdxShapeStat = idxShape.isStatic();
    const auto indicesRank = idxShape.getRank();
    CPU_NODE_ASSERT(dataSrcRank != 0LU && indicesRank != 0LU, "has incorrect input parameters ranks.");

    if (ov::is_type<ov::op::v8::Gather>(op)) {
        batchDims = static_cast<int>(ov::as_type_ptr<ov::op::v8::Gather>(op)->get_batch_dims());
        // WA for NMS->Gather construction. NMS fills part of the output blob by the -1 if these values
        // must not be taken into account. There is appropriate pass that looks for such subgraphs
        // and sets the dontReverseIndices flag.
        const auto& rti = op->get_rt_info();
        const auto& reverse = rti.find("dontReverseIndices");
        reverseIndexing = reverse == rti.end();
    } else if (ov::is_type<ov::op::v7::Gather>(op)) {
        batchDims = static_cast<int>(ov::as_type_ptr<ov::op::v7::Gather>(op)->get_batch_dims());
        reverseIndexing = false;
    } else if (ov::is_type<ov::op::internal::GatherCompressed>(op)) {
        batchDims = static_cast<int>(ov::as_type_ptr<ov::op::internal::GatherCompressed>(op)->get_batch_dims());
        reverseIndexing = true;
    }

    if (batchDims < 0) {
        batchDims += indicesRank;
    }
    CPU_NODE_ASSERT(batchDims >= 0 && batchDims <= std::min(dataSrcRank, static_cast<int>(indicesRank)),
                    "has incorrect batch_dims ",
                    batchDims,
                    "!");

    if (ov::is_type<ov::op::v0::Constant>(op->get_input_node_ptr(GATHER_AXIS))) {
        isAxisInputConst = true;
        axis = ov::as_type<ov::op::v0::Constant>(op->get_input_node_ptr(GATHER_AXIS))->cast_vector<int>()[0];
        if (axis < 0) {
            axis += dataSrcRank;
        }
        CPU_NODE_ASSERT(axis >= 0 && axis < dataSrcRank && batchDims <= axis,
                        "has incorrect input parameter axis value: ",
                        axis);
    }

    if (auto* indices = ov::as_type<ov::op::v0::Constant>(op->get_input_node_ptr(GATHER_INDICES))) {
        constIndices = indices->cast_vector<int>();
    }
}

void Gather::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty()) {
        return;
    }

    dataPrecision = getOriginalInputPrecisionAtPort(GATHER_DATA);
    outPrecision = getOriginalOutputPrecisionAtPort(0);
    if (!fusedWith.empty()) {
        outPrecision = fusedWith[fusedWith.size() - 1]->getOriginalOutputPrecisionAtPort(0);
    }

    dataTypeSize = dataPrecision.size();
    outTypeSize = outPrecision.size();

    const auto& dataDims = getInputShapeAtPort(GATHER_DATA).getDims();
    if (isAxisInputConst && isDataShapeStat) {
        axisDim = dataDims[axis];
        beforeAxisSize = std::accumulate(dataDims.begin(), dataDims.begin() + axis, 1LU, std::multiplies<>());
        betweenBatchAndAxisSize =
            std::accumulate(dataDims.begin() + batchDims, dataDims.begin() + axis, 1LU, std::multiplies<>());
        afterAxisSize = std::accumulate(dataDims.begin() + axis + 1, dataDims.end(), 1LU, std::multiplies<>());

        afterAxisSizeInBytes = afterAxisSize * dataTypeSize;
        afterAxisSizeInBytesOut = afterAxisSize * outTypeSize;
        axisAndAfterAxisSize = axisDim * afterAxisSize;
        axisAndAfterAxisSizeInBytes = axisDim * afterAxisSizeInBytes;
        srcAfterBatchSize = betweenBatchAndAxisSize * axisAndAfterAxisSize;
        srcAfterBatchSizeInBytes = betweenBatchAndAxisSize * axisAndAfterAxisSizeInBytes;
    }
    if (isDataShapeStat) {
        beforeBatchSize = std::accumulate(dataDims.begin(), dataDims.begin() + batchDims, 1LU, std::multiplies<>());
    }
    if (isIdxShapeStat) {
        const auto& idxDims = getInputShapeAtPort(GATHER_INDICES).getDims();
        specIndicesSize = std::accumulate(idxDims.begin() + batchDims, idxDims.end(), 1LU, std::multiplies<>());

        if (isDataShapeStat) {
            specIdxAndAfterAxSize = specIndicesSize * afterAxisSize;
            specIdxAndAfterAxSizeB = specIndicesSize * afterAxisSizeInBytes;
            specIdxAndAfterAxSizeBOut = specIndicesSize * afterAxisSizeInBytesOut;
            totalWork = beforeBatchSize * betweenBatchAndAxisSize * specIndicesSize * afterAxisSize;
        }
    }
    if (compressed) {
        // gatherCompressed support input precision (u4/i4/u8/i8) to output precision (f16/bf16/f32).
        if (none_of(dataPrecision, ov::element::u8, ov::element::u4, ov::element::i8, ov::element::i4)) {
            dataPrecision = ov::element::f32;
        }

        ov::element::Type scalePrecision = getOriginalInputPrecisionAtPort(GATHER_SCALE);
        if (scalePrecision != ov::element::f32) {
            scalePrecision = ov::element::f32;
        }

        if (none_of(outPrecision, ov::element::f32, ov::element::f16, ov::element::bf16)) {
            outPrecision = ov::element::f32;
        }
        scale_group_size =
            getInputShapeAtPort(GATHER_DATA).getElementsCount() / getInputShapeAtPort(GATHER_SCALE).getElementsCount();
        have_scalar_scale = getInputShapeAtPort(GATHER_SCALE).getElementsCount() == 1U;

        if (getOriginalInputsNumber() == 5U) {
            ov::element::Type zpPrecision = getOriginalInputPrecisionAtPort(GATHER_ZP);
            if (zpPrecision != ov::element::f32) {
                zpPrecision = ov::element::f32;
            }

            have_zp = true;
            have_scalar_zp = getInputShapeAtPort(GATHER_ZP).getElementsCount() == 1U;
            zp_group_size =
                getInputShapeAtPort(GATHER_DATA).getElementsCount() / getInputShapeAtPort(GATHER_ZP).getElementsCount();
            addSupportedPrimDesc({{LayoutType::ncsp, dataPrecision},
                                  {LayoutType::ncsp, ov::element::i32},
                                  {LayoutType::ncsp, ov::element::i32},
                                  {LayoutType::ncsp, scalePrecision},
                                  {LayoutType::ncsp, zpPrecision}},
                                 {{LayoutType::ncsp, outPrecision}},
                                 ref_any);
        } else {
            addSupportedPrimDesc({{LayoutType::ncsp, dataPrecision},
                                  {LayoutType::ncsp, ov::element::i32},
                                  {LayoutType::ncsp, ov::element::i32},
                                  {LayoutType::ncsp, scalePrecision}},
                                 {{LayoutType::ncsp, outPrecision}},
                                 ref_any);
        }
        return;
    }  // Implementation desc type will be redefined in the fn prepareParams if a kernel will be created.
    addSupportedPrimDesc({{LayoutType::ncsp, dataPrecision},
                          {LayoutType::ncsp, ov::element::i32},
                          {LayoutType::ncsp, ov::element::i32, isAxisInputConst}},
                         {{LayoutType::ncsp, outPrecision}},
                         ref_any);

    // Let's check for the special inPlace memory use case
    // in place only makes sense when we split by dense blocks since strided tensors are not supported by most nodes

    if (dataPrecision != outPrecision) {
        return;
    }

    if (!isAxisInputConst) {
        return;
    }

    if (batchDims != 0) {
        return;
    }

    if (constIndices.size() != 1) {
        return;
    }

    const auto& parentDims = inputShapes[0].getDims();
    const auto axisDim = parentDims[axis];
    if (Shape::UNDEFINED_DIM == axisDim) {
        return;
    }

    const auto indx = constIndices.front();
    const auto normIndex = indx < 0 ? static_cast<int64_t>(axisDim) + indx : indx;

    if (normIndex < 0 || normIndex >= static_cast<int64_t>(axisDim)) {
        return;
    }

    if (std::any_of(parentDims.begin(), parentDims.begin() + axis, [](size_t dim) {
            return dim != 1;
        })) {
        return;
    }

    addSupportedPrimDesc({{LayoutType::ncsp, dataPrecision},
                          {LayoutType::ncsp, ov::element::i32},
                          {LayoutType::ncsp, ov::element::i32, isAxisInputConst}},
                         {{LayoutType::ncsp, dataPrecision, false, GATHER_DATA}},
                         unknown);
}

void Gather::createPrimitive() {
    if (isInPlace()) {
        return;
    }
    m_threads_num = parallel_get_max_threads();
#if defined(OPENVINO_ARCH_X86_64)
    uint64_t idxElPerVec = 1;
    if (!isDynamicNode()) {
        if (x64::mayiuse(x64::avx512_core)) {
            idxElPerVec = x64::cpu_isa_traits_t<x64::avx512_core>::vlen / idxTypeSize;
        } else if (x64::mayiuse(x64::avx2)) {
            idxElPerVec = x64::cpu_isa_traits_t<x64::avx2>::vlen / idxTypeSize;
        } else {
            idxElPerVec = 1;
        }
    }
    // Gather instruction is not supported by SSE.
    if ((x64::mayiuse(x64::avx512_core) || x64::mayiuse(x64::avx2)) &&
        (isDynamicNode() || afterAxisSize == 1 ||
         (afterAxisSize <= idxElPerVec &&
          (x64::mayiuse(x64::avx512_core) || (x64::mayiuse(x64::avx2) && dataTypeSize == 4))))) {
        jGatherConfParams jcp;
        jcp.dataTypeSize = dataTypeSize;
        jcp.in_prec = dataPrecision;
        jcp.out_prec = outPrecision;
        jcp.reverseIndexing = reverseIndexing;
        jcp.dynamicShapes = isDynamicNode();
        jcp.batchDims = batchDims;
        if (!jcp.dynamicShapes) {
            jcp.beforeAxisSize = beforeAxisSize;
            jcp.specIdxSize = specIndicesSize;
            jcp.afterAxisSize = afterAxisSize;
        } else {
            if (isDataShapeStat && isAxisInputConst) {
                jcp.beforeAxisSize = beforeAxisSize;
                jcp.afterAxisSize = afterAxisSize;
            }
            if (isIdxShapeStat) {
                jcp.specIdxSize = specIndicesSize;
            }
        }

        if (x64::mayiuse(x64::avx512_core)) {
            jitKernel = std::make_shared<jitUniGatherKernel<x64::avx512_core>>(jcp);
        } else if (x64::mayiuse(x64::avx2)) {
            jitKernel = std::make_shared<jitUniGatherKernel<x64::avx2>>(jcp);
        }
        if (jitKernel) {
            jitKernel->create_ker();

            if (!isDynamicNode()) {
                const uint64_t dataElPerVec = jitKernel->getDataElPerVec();
                const uint64_t wpt = ((totalWork / dataElPerVec) / m_threads_num + 1) * dataElPerVec;
                execParamsPerThread.resize(m_threads_num);

                parallel_nt(m_threads_num, [&](const int ithr, [[maybe_unused]] const int nthr) {
                    const uint64_t dstStart = std::min(wpt * ithr, totalWork);
                    const uint64_t dstEnd = std::min(wpt * (ithr + 1), totalWork);

                    auto& p = execParamsPerThread[ithr];
                    p.workAmount = dstEnd - dstStart;
                    p.dstStart = dstStart;
                    p.specIdxInBytes.resize(dataElPerVec);
                    p.idxBatchSumInBytes.resize(dataElPerVec);
                    p.dataBeforeAxisSumInBytes.resize(dataElPerVec);
                    p.betweenBatchAndAxisIter = (dstStart / specIndicesSize) % betweenBatchAndAxisSize;
                    for (uint64_t j = 0LU; j < dataElPerVec; j++) {
                        p.specIdxInBytes[j] = (((dstStart + j) / afterAxisSize) % specIndicesSize) * idxTypeSize;
                        p.idxBatchSumInBytes[j] =
                            ((dstStart + j) / (betweenBatchAndAxisSize * specIndicesSize * afterAxisSize)) *
                            specIndicesSize * idxTypeSize;
                        p.dataBeforeAxisSumInBytes[j] =
                            ((dstStart + j) / (specIndicesSize * afterAxisSize)) * axisAndAfterAxisSizeInBytes;
                    }
                    initShortParams(p, dstStart);
                });
            }
        }
    }
#endif
    Node::createPrimitive();
}

bool Gather::needPrepareParams() const {
    if (isInPlace()) {
        return false;
    }
    bool result = inputShapesModified();
    if (!isAxisInputConst) {
        result = result || axis != (getSrcDataAtPortAs<const int32_t>(GATHER_AXIS))[0];
    }
    return result;
}

void Gather::prepareParams() {
    auto dataMemPtr = getSrcMemoryAtPort(GATHER_DATA);
    if (!dataMemPtr || !dataMemPtr->isDefined()) {
        CPU_NODE_THROW("has undefined input data memory.");
    }
    auto idxMemPtr = getSrcMemoryAtPort(GATHER_INDICES);
    if (!idxMemPtr || !idxMemPtr->isDefined()) {
        CPU_NODE_THROW("has undefined input indices memory.");
    }
    if (getSelectedPrimitiveDescriptor() == nullptr) {
        CPU_NODE_THROW("has unidentified preferable primitive descriptor.");
    }

    // short 1D vector fast execution impl (typical in shape infer subgraph)
    canOptimize1DCase = false;
    if (dataSrcRank <= 1 && dataMemPtr->getDesc().getPrecision() == ov::element::i32) {
        const auto& dataDims = dataMemPtr->getStaticDims();
        const auto& idxDims = idxMemPtr->getStaticDims();
        if ((dataDims.empty() || (dataDims.size() == 1 && dataDims[0] <= 64)) &&
            (idxDims.empty() || (idxDims.size() == 1 && idxDims[0] <= 64))) {
            canOptimize1DCase = true;
            return;
        }
    }

    if (!isAxisInputConst) {
        axis = (getSrcDataAtPortAs<const int32_t>(GATHER_AXIS))[0];
        if (axis < 0) {
            axis += dataSrcRank;
        }
        CPU_NODE_ASSERT(axis >= 0 && axis < dataSrcRank && batchDims <= axis,
                        "has incorrect input parameter axis value: ",
                        axis);
    }

    if (!isDataShapeStat || !isAxisInputConst) {
        const auto& dataDims = dataMemPtr->getStaticDims();
        axisDim = dataDims[axis];
        beforeBatchSize = std::accumulate(dataDims.begin(), dataDims.begin() + batchDims, 1LU, std::multiplies<>());
        betweenBatchAndAxisSize =
            std::accumulate(dataDims.begin() + batchDims, dataDims.begin() + axis, 1LU, std::multiplies<>());
        afterAxisSize = std::accumulate(dataDims.begin() + axis + 1, dataDims.end(), 1LU, std::multiplies<>());

        afterAxisSizeInBytes = afterAxisSize * dataTypeSize;
        afterAxisSizeInBytesOut = afterAxisSize * outTypeSize;
        axisAndAfterAxisSize = axisDim * afterAxisSize;
        axisAndAfterAxisSizeInBytes = axisDim * afterAxisSizeInBytes;
        srcAfterBatchSize = betweenBatchAndAxisSize * axisAndAfterAxisSize;
        srcAfterBatchSizeInBytes = betweenBatchAndAxisSize * axisAndAfterAxisSizeInBytes;

        if (isIdxShapeStat) {
            specIdxAndAfterAxSize = specIndicesSize * afterAxisSize;
            specIdxAndAfterAxSizeB = specIndicesSize * afterAxisSizeInBytes;
            specIdxAndAfterAxSizeBOut = specIndicesSize * afterAxisSizeInBytesOut;
            totalWork = beforeBatchSize * betweenBatchAndAxisSize * specIndicesSize * afterAxisSize;
        }
    }

    if (!isIdxShapeStat) {
        const auto& idxDims = idxMemPtr->getStaticDims();
        specIndicesSize = std::accumulate(idxDims.begin() + batchDims, idxDims.end(), 1LU, std::multiplies<>());

        specIdxAndAfterAxSize = specIndicesSize * afterAxisSize;
        specIdxAndAfterAxSizeB = specIndicesSize * afterAxisSizeInBytes;
        specIdxAndAfterAxSizeBOut = specIndicesSize * afterAxisSizeInBytesOut;
        totalWork = beforeBatchSize * betweenBatchAndAxisSize * specIndicesSize * afterAxisSize;
    }

#if defined(OPENVINO_ARCH_X86_64)
    const auto& selectedPD = getSelectedPrimitiveDescriptor();
    if (jitKernel && jitKernel->isSupportedConfiguration(afterAxisSize)) {
        if (x64::mayiuse(x64::avx512_core)) {
            selectedPD->setImplementationType(jit_avx512);
        } else if (x64::mayiuse(x64::avx2)) {
            selectedPD->setImplementationType(jit_avx2);
        }
    }
#endif
}

void Gather::execute([[maybe_unused]] const dnnl::stream& strm) {
    if (isInPlace()) {
        return;
    }

    if (canOptimize1DCase) {
        exec1DCase();
        return;
    }

    if (compressed) {
        execCompressed();
        return;
    }
#if defined(OPENVINO_ARCH_X86_64)
    if (jitKernel && jitKernel->isSupportedConfiguration(afterAxisSize)) {
        const void* srcIndices = getSrcDataAtPort(GATHER_INDICES);
        const void* srcData = getSrcDataAtPort(GATHER_DATA);
        auto* dstData = getDstDataAtPortAs<uint8_t>(0);

        const uint64_t dataElPerVec = jitKernel->getDataElPerVec();

        auto threadBody = [&](const int ithr, [[maybe_unused]] const int nthr) {
            auto& p = execParamsPerThread[ithr];
            auto arg = gatherJitExecArgs();

            arg.src = srcData;
            arg.dst = dstData + p.dstStart * outTypeSize;
            arg.indices = srcIndices;
            arg.start = &p.dstStart;
            arg.axisDim = &axisDim;
            arg.afterAxSize = afterAxisSize;
            arg.axisAndAfterAxisSizeB = &axisAndAfterAxisSizeInBytes;
            arg.srcAfterBatchSizeB = &srcAfterBatchSizeInBytes;
            arg.betweenBatchAndAxisSize = &betweenBatchAndAxisSize;
            arg.specIndicesSize = &specIndicesSize;
            arg.workAmount = p.workAmount;
            arg.specIdxB = p.specIdxInBytes.data();
            arg.idxBatchSumB = p.idxBatchSumInBytes.data();
            arg.dataBeforeAxisSumB = p.dataBeforeAxisSumInBytes.data();
            arg.betweenBatchAndAxisIter = p.betweenBatchAndAxisIter;

            const uint64_t idxElPerVec = jitKernel->getIdxElPerVec();

            if (afterAxisSize == 1 && specIndicesSize < idxElPerVec) {  // Elementwise short case.
                arg.permIdxMask = p.permIdxMask.data();
                arg.beforeAxisDiff = p.srcBeforeAxisDiff.data();
            } else if (afterAxisSize > 1 && afterAxisSize <= dataElPerVec) {  // Blocked short case.
                arg.afterAxIdxB = p.afterAxIdxInBytes.data();
                arg.specIdxDiff = p.specIdxDiff.data();
                arg.beforeAxisDiff = p.srcBeforeAxisDiff.data();
                arg.beforeAxisPermMask = p.beforeAxPermMask.data();
                arg.afterAxisPermMask = p.afterAxPermMask.data();
                arg.afterAxisSize = &afterAxisSize;
                arg.specIdxAndAfterAxIterB = p.specIdxAndAfterAxIterB;
                arg.specIdxAndAfterAxSizeB = specIdxAndAfterAxSizeB;
            }

            (*jitKernel)(&arg);
        };

        parallel_nt(m_threads_num, threadBody);

        return;
    }
#endif
    execReference();
}

void Gather::executeDynamicImpl([[maybe_unused]] const dnnl::stream& strm) {
    if (isInPlace()) {
        return;
    }
    if (canOptimize1DCase) {
        exec1DCase();
        return;
    }

    if (compressed) {
        execCompressed();
        return;
    }

#if defined(OPENVINO_ARCH_X86_64)
    if (jitKernel && jitKernel->isSupportedConfiguration(afterAxisSize)) {
        const void* srcIndices = getSrcDataAtPort(GATHER_INDICES);
        const void* srcData = getSrcDataAtPort(GATHER_DATA);
        auto* dstData = getDstDataAtPortAs<uint8_t>(0);

        const uint64_t dataElPerVec = jitKernel->getDataElPerVec();

        auto threadBody = [&](const int ithr, const int nthr) {
            const uint64_t wpt = ((totalWork / dataElPerVec) / nthr + 1) * dataElPerVec;
            const uint64_t start = std::min(wpt * ithr, totalWork);
            const uint64_t end = std::min(wpt * (ithr + 1), totalWork);
            const uint64_t workAmount = end - start;  // how many elements processed in this thread

            auto arg = gatherJitExecArgs();

            arg.src = srcData;
            arg.dst = dstData + afterAxisSizeInBytesOut * start;
            arg.indices = srcIndices;
            arg.start = &start;
            arg.axisDim = &axisDim;
            arg.afterAxSize = afterAxisSize;
            arg.axisAndAfterAxisSizeB = &axisAndAfterAxisSizeInBytes;
            arg.srcAfterBatchSizeB = &srcAfterBatchSizeInBytes;
            arg.betweenBatchAndAxisSize = &betweenBatchAndAxisSize;
            arg.specIndicesSize = &specIndicesSize;
            arg.workAmount = workAmount;

            const uint64_t idxElPerVec = jitKernel->getIdxElPerVec();
            int permIdxMask[16];
            int beforeAxisDiff[16];
            if (afterAxisSize == 1 && specIndicesSize < idxElPerVec) {
                permIdxMask[0] = idxElPerVec - specIndicesSize;
                int div = idxElPerVec / specIndicesSize;
                int remainder = idxElPerVec % specIndicesSize;
                for (uint64_t i = 1; i < idxElPerVec; i++) {
                    permIdxMask[i] = permIdxMask[i - 1] + 1;
                    if (static_cast<uint64_t>(permIdxMask[i]) == idxElPerVec) {
                        permIdxMask[i] = idxElPerVec - specIndicesSize;
                    }
                }
                for (uint64_t i = 0; i < idxElPerVec; i++) {
                    if (((start + i) % specIndicesSize) < (specIndicesSize - remainder)) {
                        beforeAxisDiff[i] = axisDim * div;
                    } else {
                        beforeAxisDiff[i] = axisDim * (div + 1);
                    }
                }
                arg.permIdxMask = permIdxMask;
                arg.beforeAxisDiff = beforeAxisDiff;
            }

            (*jitKernel)(&arg);
        };

        parallel_nt(m_threads_num, threadBody);

        return;
    }
#endif
    execReference();
}

void Gather::initShortParams(threadExecParams& p, const uint64_t start) {
    CPU_NODE_ASSERT(jitKernel, "has uninitialized kernel in function initShortParams.");
    const uint64_t idxElPerVec = jitKernel->getIdxElPerVec();

    if (afterAxisSize == 1) {  // Elementwise gather.
        if (specIndicesSize >= idxElPerVec) {
            return;  // Is not a short case.
        }

        p.permIdxMask.resize(idxElPerVec);
        p.srcBeforeAxisDiff.resize(idxElPerVec);

        p.permIdxMask[0] = idxElPerVec - specIndicesSize;
        for (uint64_t i = 1; i < idxElPerVec; i++) {
            p.permIdxMask[i] = p.permIdxMask[i - 1] + 1;
            if (static_cast<uint64_t>(p.permIdxMask[i]) == idxElPerVec) {
                p.permIdxMask[i] = idxElPerVec - specIndicesSize;
            }
        }

        const int div = idxElPerVec / specIndicesSize;
        const int remainder = idxElPerVec % specIndicesSize;
        for (uint64_t i = 0; i < idxElPerVec; i++) {
            if (((start + i) % specIndicesSize) < (specIndicesSize - remainder)) {
                p.srcBeforeAxisDiff[i] = axisDim * div;
            } else {
                p.srcBeforeAxisDiff[i] = axisDim * (div + 1);
            }
        }
    } else {  // Blocked gather.
        if (afterAxisSize > idxElPerVec) {
            return;  // Is not a short case.
        }

        p.afterAxIdxInBytes.resize(idxElPerVec);
        p.afterAxPermMask.resize(idxElPerVec);
        p.beforeAxPermMask.resize(idxElPerVec);
        p.specIdxDiff.resize(idxElPerVec);
        p.srcBeforeAxisDiff.resize(idxElPerVec);

        int secondStart = start + idxElPerVec;
        for (uint64_t i = 0; i < idxElPerVec; i++) {
            p.afterAxIdxInBytes[i] = (start + i) % afterAxisSize;
            p.specIdxDiff[i] =
                (((secondStart + i) / afterAxisSize) % specIndicesSize) * idxTypeSize - p.specIdxInBytes[i];
            if (p.specIdxDiff[i] < 0) {
                p.specIdxDiff[i] += specIndicesSize * idxTypeSize;
            }
            p.srcBeforeAxisDiff[i] =
                ((start + i + idxElPerVec) / (specIndicesSize * afterAxisSize)) * axisAndAfterAxisSizeInBytes -
                ((start + i) / (specIndicesSize * afterAxisSize)) * axisAndAfterAxisSizeInBytes;

            p.afterAxIdxInBytes[i] *= dataTypeSize;
            p.afterAxPermMask[i] = idxElPerVec - afterAxisSize + i;
            for (size_t j = 0LU; j < 6LU; j++) {
                if (static_cast<uint64_t>(p.afterAxPermMask[i]) >= idxElPerVec) {
                    p.afterAxPermMask[i] -= afterAxisSize;
                }
            }
        }
        if (specIndicesSize * afterAxisSize < idxElPerVec) {
            p.beforeAxPermMask[0] = idxElPerVec - specIndicesSize * afterAxisSize;
            for (uint64_t i = 1; i < idxElPerVec; i++) {
                p.beforeAxPermMask[i] = p.beforeAxPermMask[i - 1] + 1;
                if (static_cast<uint64_t>(p.beforeAxPermMask[i]) == idxElPerVec) {
                    p.beforeAxPermMask[i] = idxElPerVec - specIndicesSize * afterAxisSize;
                }
            }
        }

        p.specIdxAndAfterAxIterB = (start * dataTypeSize) % specIdxAndAfterAxSizeB;
    }
}

template <typename OUT_TYPE, int8_t get4Bit(const uint8_t&, bool)>
void Gather::execCompressed4Bit() {
    const auto* srcIndices = getSrcDataAtPortAs<const int32_t>(GATHER_INDICES);
    const auto* srcData = getSrcDataAtPortAs<const uint8_t>(GATHER_DATA);
    auto* dstData = getDstDataAtPortAs<OUT_TYPE>(0);

    // zp/scale
    float const_zp = 0;
    const auto* zp = have_zp ? getSrcDataAtPortAs<float_t>(GATHER_ZP) : &const_zp;
    const auto* scale = getSrcDataAtPortAs<float_t>(GATHER_SCALE);

    const size_t dstAfterBatchSize = betweenBatchAndAxisSize * specIdxAndAfterAxSize;
    parallel_for2d(beforeBatchSize, specIndicesSize, [&](const size_t b, const size_t j) {
        int ii = srcIndices[b * specIndicesSize + j];
        if (ii < 0) {
            if (reverseIndexing) {
                ii += axisDim;
            } else {
                ii = axisDim;
            }
        }
        const size_t idx = ii;
        const size_t c2 = dstAfterBatchSize * b + afterAxisSize * j;
        if (idx < static_cast<size_t>(axisDim)) {
            size_t c1 = srcAfterBatchSize * b + afterAxisSize * idx;
            for (size_t i = 0; i < betweenBatchAndAxisSize; i++) {
                size_t srcIdx = c1 + axisAndAfterAxisSize * i;
                size_t dstIdx = c2 + specIdxAndAfterAxSize * i;

                OUT_TYPE* pdst = &dstData[dstIdx];

                size_t p = srcIdx;
                size_t dst_idx = 0;

                // heuristic:
                // ((isAxisInputConst && axis == 0) && (cond1 || cond2)) take >99% probability
                bool processed = false;
                if (isAxisInputConst && axis == 0) {
                    bool cond1 = have_zp && zp_group_size == scale_group_size;
                    bool cond2 = (!have_zp) || have_scalar_zp;
                    bool cond3 = have_scalar_scale && cond2;
                    if (cond3) {
                        processed = true;
                        for (; p < srcIdx + afterAxisSize; p++) {
                            auto val = srcData[p >> 1];
                            pdst[dst_idx] = static_cast<OUT_TYPE>((get4Bit(val, p % 2) - zp[0]) * scale[0]);
                            dst_idx++;
                        }
                    } else if (cond1 || cond2) {
                        processed = true;
                        for (; p < srcIdx + afterAxisSize; p += scale_group_size) {
                            const auto& cur_scale = scale[p / scale_group_size];
                            const auto& cur_zp = cond2 ? zp[0] : zp[p / zp_group_size];
                            for (size_t g = p; g < p + scale_group_size; g++) {
                                auto val = srcData[g >> 1];
                                pdst[dst_idx] = static_cast<OUT_TYPE>((get4Bit(val, g % 2) - cur_zp) * cur_scale);
                                dst_idx++;
                            }
                        }
                    }
                }

                // Reference
                if (!processed) {
                    for (; p < srcIdx + afterAxisSize; p++) {
                        auto val = srcData[p >> 1];
                        const size_t scale_offset = p / scale_group_size;
                        auto cur_zp = have_zp ? zp[p / zp_group_size] : 0;
                        pdst[dst_idx] = static_cast<OUT_TYPE>((get4Bit(val, p % 2) - cur_zp) * scale[scale_offset]);
                        dst_idx++;
                    }
                }
            }
        } else {
            for (size_t i = 0; i < betweenBatchAndAxisSize; i++) {
                size_t dstIdx = c2 + specIdxAndAfterAxSize * i;
                for (size_t p = 0; p < afterAxisSize; p++) {
                    dstData[dstIdx] = 0;
                }
            }
        }
    });
}

template <typename OUT_TYPE, typename IN_TYPE>
void Gather::execCompressed8Bit() {
    const auto* srcIndices = getSrcDataAtPortAs<const int32_t>(GATHER_INDICES);
    const auto* srcData = getSrcDataAtPortAs<const IN_TYPE>(GATHER_DATA);
    auto* dstData = getDstDataAtPortAs<OUT_TYPE>(0);

    // zp/scale
    float const_zp = 0;
    const auto* zp = have_zp ? getSrcDataAtPortAs<float_t>(GATHER_ZP) : &const_zp;
    const auto* scale = getSrcDataAtPortAs<float_t>(GATHER_SCALE);

    const size_t dstAfterBatchSize = betweenBatchAndAxisSize * specIdxAndAfterAxSize;

    parallel_for2d(beforeBatchSize, specIndicesSize, [&](const size_t b, const size_t j) {
        int ii = srcIndices[b * specIndicesSize + j];
        if (ii < 0) {
            if (reverseIndexing) {
                ii += axisDim;
            } else {
                ii = axisDim;
            }
        }
        const size_t idx = ii;
        const size_t c2 = dstAfterBatchSize * b + afterAxisSize * j;
        if (idx < static_cast<size_t>(axisDim)) {
            size_t c1 = srcAfterBatchSize * b + afterAxisSize * idx;
            for (size_t i = 0; i < betweenBatchAndAxisSize; i++) {
                size_t srcIdx = c1 + axisAndAfterAxisSize * i;
                size_t dstIdx = c2 + specIdxAndAfterAxSize * i;

                OUT_TYPE* pdst = &dstData[dstIdx];

                size_t p = srcIdx;
                size_t dst_idx = 0;

                // heuristic:
                // ((isAxisInputConst && axis == 0) && (cond1 || cond2)) take >99% probability
                bool processed = false;
                if (isAxisInputConst && axis == 0) {
                    bool cond1 = have_zp && zp_group_size == scale_group_size;
                    bool cond2 = (!have_zp) || have_scalar_zp;
                    bool cond3 = have_scalar_scale && cond2;
                    if (cond3) {
                        processed = true;
                        for (; p < srcIdx + afterAxisSize; p++) {
                            pdst[dst_idx] = static_cast<OUT_TYPE>((static_cast<float>(srcData[p]) - zp[0]) * scale[0]);
                            dst_idx++;
                        }
                    } else if (cond1 || cond2) {
                        processed = true;
                        for (; p < srcIdx + afterAxisSize; p += scale_group_size) {
                            const auto& cur_scale = scale[p / scale_group_size];
                            const auto& cur_zp = cond2 ? zp[0] : zp[p / zp_group_size];
                            for (size_t g = p; g < p + scale_group_size; g++) {
                                pdst[dst_idx] =
                                    static_cast<OUT_TYPE>((static_cast<float>(srcData[g]) - cur_zp) * cur_scale);
                                dst_idx++;
                            }
                        }
                    }
                }

                // Reference
                if (!processed) {
                    for (; p < srcIdx + afterAxisSize; p++) {
                        const size_t scale_offset = p / scale_group_size;
                        auto cur_zp = have_zp ? zp[p / zp_group_size] : 0;
                        pdst[dst_idx] =
                            static_cast<OUT_TYPE>((static_cast<float>(srcData[p]) - cur_zp) * scale[scale_offset]);
                        dst_idx++;
                    }
                }
            }
        } else {
            for (size_t i = 0; i < betweenBatchAndAxisSize; i++) {
                size_t dstIdx = c2 + specIdxAndAfterAxSize * i;
                for (size_t p = 0; p < afterAxisSize; p++) {
                    dstData[dstIdx] = 0;
                }
            }
        }
    });
}

int8_t Gather::get_i4(const uint8_t& val, bool high) {
    if (high) {
        if (val & 0x80) {
            return static_cast<int8_t>((val >> 4) | 0xf8);
        }
        return static_cast<int8_t>(val >> 4);
    }
    if (val & 0x8) {
        // Just fill in the high 4 bits with 1
        return static_cast<int8_t>(val | 0xf8);
    }
    return static_cast<int8_t>(val & 0xF);
}

int8_t Gather::get_u4(const uint8_t& val, bool high) {
    if (high) {
        return static_cast<int8_t>((val >> 4) & 0xF);
    }
    return static_cast<int8_t>(val & 0xF);
}

struct ExecCompressedContext {
    Gather* node;
    ov::element::Type inType;
};

template <typename OUT_PRECISION>
struct ExecCompressedDispatcher {
    void operator()(ExecCompressedContext& ctx) {
        if (ctx.inType.bitwidth() == 8) {
            ExecCompressed8Bit_dispatch(ctx);
        } else {
            ExecCompressed4Bit_dispatch(ctx);
        }
    }

    template <typename IN_PRECISION>
    struct ExecCompressed8BitDispatcher {
        void operator()(ExecCompressedContext& ctx) {
            ctx.node->execCompressed8Bit<OUT_PRECISION, IN_PRECISION>();
        }
    };

private:
    void ExecCompressed8Bit_dispatch([[maybe_unused]] ExecCompressedContext& ctx) {
        OV_SWITCH(intel_cpu,
                  ExecCompressed8BitDispatcher,
                  ctx,
                  ctx.inType,
                  OV_CASE(ov::element::u8, uint8_t),
                  OV_CASE(ov::element::i8, int8_t));
    }
    void ExecCompressed4Bit_dispatch(ExecCompressedContext& ctx) {
        switch (ctx.inType) {
        case ov::element::u4:
            return ctx.node->execCompressed4Bit<OUT_PRECISION, Gather::get_u4>();
        case ov::element::i4:
            return ctx.node->execCompressed4Bit<OUT_PRECISION, Gather::get_i4>();
        default:
            break;
        }
    }
};

void Gather::execCompressed() {
    auto in_precison = getParentEdgeAt(GATHER_DATA)->getMemoryPtr()->getPrecision();
    auto out_precision = getChildEdgeAt(0)->getMemoryPtr()->getPrecision();
    ExecCompressedContext ctx{this, in_precison};

    OV_SWITCH(intel_cpu,
              ExecCompressedDispatcher,
              ctx,
              out_precision,
              OV_CASE(ov::element::f32, float),
              OV_CASE(ov::element::bf16, ov::bfloat16),
              OV_CASE(ov::element::f16, ov::float16));
}

void Gather::execReference() {
    const auto* srcIndices = getSrcDataAtPortAs<const int32_t>(GATHER_INDICES);
    const auto* srcData = getSrcDataAtPortAs<const uint8_t>(GATHER_DATA);
    auto* dstData = getDstDataAtPortAs<uint8_t>(0);

    const size_t dstAfterBatchSize = betweenBatchAndAxisSize * specIdxAndAfterAxSizeBOut;
    parallel_for2d(beforeBatchSize, specIndicesSize, [&](const size_t b, const size_t j) {
        int ii = srcIndices[b * specIndicesSize + j];
        if (ii < 0) {
            if (reverseIndexing) {
                ii += axisDim;
            } else {
                ii = axisDim;
            }
        }
        const size_t idx = ii;
        const size_t c2 = dstAfterBatchSize * b + afterAxisSizeInBytesOut * j;
        if (idx < static_cast<size_t>(axisDim)) {
            size_t c1 = srcAfterBatchSizeInBytes * b + afterAxisSizeInBytes * idx;
            for (size_t i = 0; i < betweenBatchAndAxisSize; i++) {
                size_t srcIdx = c1 + axisAndAfterAxisSizeInBytes * i;
                size_t dstIdx = c2 + specIdxAndAfterAxSizeBOut * i;

                if (dataPrecision == outPrecision) {
                    cpu_memcpy(&dstData[dstIdx], &srcData[srcIdx], afterAxisSizeInBytes);
                } else {
                    cpu_convert(&srcData[srcIdx], &dstData[dstIdx], dataPrecision, outPrecision, afterAxisSize);
                }
            }
        } else {
            for (size_t i = 0; i < betweenBatchAndAxisSize; i++) {
                memset(&dstData[c2 + specIdxAndAfterAxSizeBOut * i], 0, afterAxisSizeInBytesOut);
            }
        }
    });
}

void Gather::exec1DCase() {
    DEBUG_LOG(getName(), " exec1DCase");
    auto* pdst = getDstDataAtPortAs<uint32_t>(0);
    auto srcMemPtr = getSrcMemoryAtPort(GATHER_DATA);
    auto idxMemPtr = getSrcMemoryAtPort(GATHER_INDICES);
    const auto* psrc = srcMemPtr->getDataAs<const uint32_t>();
    const auto* pidx = idxMemPtr->getDataAs<int32_t>();

    const auto& idxDims = idxMemPtr->getStaticDims();
    const auto idxCnt = (idxDims.empty()) ? 1 : idxDims[0];
    auto axisDim = srcMemPtr->getStaticDims()[0];
    for (size_t i = 0; i < idxCnt; i++) {
        auto ii = pidx[i];
        if (ii < 0) {
            if (reverseIndexing) {
                ii += axisDim;
            } else {
                ii = axisDim;
            }
        }
        pdst[i] = psrc[ii];
    }
}

bool Gather::created() const {
    return getType() == Type::Gather;
}

bool Gather::neverExecute() const {
    return isInPlace() || Node::neverExecute();
}

bool Gather::isExecutable() const {
    return !isInPlace() && Node::isExecutable();
}

void Gather::resolveInPlaceEdges(Edge::LOOK look) {
    if (!(look & Edge::LOOK_UP) || !isInPlace()) {
        Node::resolveInPlaceEdges(look);
        return;
    }

    auto* selected_pd = getSelectedPrimitiveDescriptor();
    CPU_NODE_ASSERT(selected_pd, "Preferable primitive descriptor is not set.");
    constexpr size_t outputPort = 0;

    const auto& config = selected_pd->getConfig();
    size_t inplaceInpIndx = selected_pd->getConfig().outConfs[outputPort].inPlace();
    const auto baseDim = inputShapes.front().getDims()[axis];
    CPU_NODE_ASSERT(baseDim != Shape::UNDEFINED_DIM, "can not use inPlace memory with splitting on dynamic dimention");
    auto baseMemBlock = getParentEdgeAt(inplaceInpIndx)->getMemory().getMemoryBlock();
    const auto index = constIndices.front();
    const ptrdiff_t offset = index < 0 ? baseDim + index : index;
    const auto& childEdges = getChildEdgesAtPort(outputPort);
    for (const auto& childEdge : childEdges) {
        CPU_NODE_ASSERT(childEdge->getStatus() == Edge::Status::NotAllocated, "Unexpected edge status");

        auto memBlock = std::make_shared<PartitionedMemoryBlock>(baseMemBlock, baseDim, offset);
        auto newMem = std::make_shared<Memory>(getEngine(), config.outConfs[outputPort].getMemDesc(), memBlock);

        childEdge->reuse(newMem);
    }
}

bool Gather::canFuse(const NodePtr& node) const {
    if (node->getType() != Type::Convert) {
        return false;
    }
    return any_of(node->getOriginalInputPrecisionAtPort(0), element::f16, element::bf16) &&
           node->getOriginalOutputPrecisionAtPort(0) == ov::element::f32;
}

}  // namespace ov::intel_cpu::node
