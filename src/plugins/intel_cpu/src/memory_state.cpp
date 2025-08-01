// Copyright (C) 2018-2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "memory_state.h"

#include <nodes/common/cpu_convert.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <oneapi/dnnl/dnnl_common.hpp>
#include <string>
#include <utility>
#include <vector>

#include "cpu_memory.h"
#include "cpu_tensor.h"
#include "cpu_types.h"
#include "dnnl_extension_utils.h"
#include "memory_desc/blocked_memory_desc.h"
#include "memory_desc/cpu_blocked_memory_desc.h"
#include "memory_desc/cpu_memory_desc.h"
#include "memory_desc/cpu_memory_desc_utils.h"
#include "nodes/kernels/scaled_attn/attn_quant.hpp"
#include "openvino/core/except.hpp"
#include "openvino/core/parallel.hpp"
#include "openvino/core/type/element_type.hpp"
#include "openvino/runtime/itensor.hpp"
#include "openvino/runtime/so_ptr.hpp"
#include "utils/general_utils.h"
#include "utils/plain_tensor.hpp"

using namespace ov::Extensions::Cpu::XARCH;

namespace ov::intel_cpu {

VariableStateBase::VariableStateBase(const std::string& name, MemoryDescPtr external_desc)
    : IVariableState{name},
      m_external_desc{std::move(external_desc)} {}

MemoryDescPtr VariableStateBase::to_static(const MemoryDescPtr& desc) {
    if (!desc->isDefined()) {
        auto&& current_dims = desc->getShape().getDims();
        VectorDims new_dims(current_dims.size());
        std::transform(current_dims.begin(), current_dims.end(), new_dims.begin(), [](Dim x) {
            return x == Shape::UNDEFINED_DIM ? 0 : x;
        });

        return desc->cloneWithNewDims(new_dims, true);
    }
    return desc;
}

const dnnl::engine& VariableStateBase::get_engine() {
    static const dnnl::engine eng(dnnl::engine::kind::cpu, 0);
    return eng;
}

void VariableStateBase::set_state_impl(const ov::SoPtr<ov::ITensor>& state) {
    auto state_desc = MemoryDescUtils::generateCpuBlockedMemoryDesc(state);

    const auto& shape = state_desc->getShape();

    if (input_mem()->getShape() != shape) {
        auto new_desc = internal_desc()->cloneWithNewDims(shape.getStaticDims());
        input_mem()->redefineDesc(new_desc);
    }

    auto* src = state->data();

    Memory mem(get_engine(), state_desc, src);
    input_mem()->load(mem, true, false);
    reset_state_flag = false;
}

void VariableStateBase::set_state(const ov::SoPtr<ov::ITensor>& state) {
    set_state_impl(state);
    reset_state_flag = false;
}

ov::SoPtr<ov::ITensor> VariableStateBase::get_state() const {
    const auto& current_dims = internal_state_mem()->getStaticDims();
    auto current_ext_desc = m_external_desc->cloneWithNewDims(current_dims);
    auto current_internal_desc = internal_state_mem()->getDescPtr();

    if (current_ext_desc->isCompatible(*current_internal_desc)) {
        return std::make_shared<Tensor>(internal_state_mem());
    }

    // test precision
    {
        auto internal_prc = current_internal_desc->getPrecision();
        auto tmp_desc = current_ext_desc->cloneWithNewPrecision(internal_prc);
        if (tmp_desc->isCompatible(*current_internal_desc)) {
            auto mem = std::make_shared<Memory>(get_engine(), current_ext_desc);
            size_t elements_to_convert =
                internal_state_mem()->getDescWithType<BlockedMemoryDesc>()->getPaddedElementsCount();
            auto external_prc = current_ext_desc->getPrecision();

            cpu_convert(internal_state_mem()->getData(),
                        mem->getData(),
                        internal_prc,
                        external_prc,
                        elements_to_convert);
            return std::make_shared<Tensor>(mem);
        }
    }

    // reorder
    auto mem = std::make_shared<Memory>(get_engine(), current_ext_desc);
    mem->load(*(internal_state_mem()), true, false);
    return std::make_shared<Tensor>(mem);
}

void VariableStateBase::reset() {
    reset_impl();
    reset_state_flag = true;
}

bool VariableStateBase::is_reset_state() const {
    return reset_state_flag;
}

void VariableStateBase::commit() {
    commit_impl();
    reset_state_flag = false;
}

VariableStateDoubleBuffer::VariableStateDoubleBuffer(const std::string& name,
                                                     const MemoryPtr& first_buffer,
                                                     const MemoryPtr& second_buffer,
                                                     const MemoryDescPtr& external_desc)
    : VariableStateBase(name, external_desc) {
    OPENVINO_ASSERT(first_buffer && second_buffer);
    reset_prime_mem(first_buffer);
    reset_second_mem(second_buffer);
    m_internal_desc = prime_mem()->getDescPtr();
    auto&& shape = m_internal_desc->getShape();
    // TODO what if by some reason we already have internal static state while the node is dynamic, is it even possible?

    if (shape.isStatic()) {
        prime_mem()->nullify();
    } else {
        // in the case of the original desc has dynamic shape we create an empty tensor
        auto new_desc = to_static(m_internal_desc);
        prime_mem()->redefineDesc(new_desc);
    }
}

void VariableStateDoubleBuffer::reset_impl() {
    auto new_desc = to_static(m_internal_desc);
    for (auto&& mem : m_internal_mem) {
        if (mem) {
            mem->redefineDesc(new_desc);
            mem->nullify();
        }
    }
}

void VariableStateDoubleBuffer::commit_impl() {
    buffer_num ^= 0x01;
}

MemoryPtr VariableStateDoubleBuffer::input_mem() {
    return prime_mem();
}

MemoryPtr VariableStateDoubleBuffer::output_mem() {
    return second_mem();
}

MemoryDescPtr VariableStateDoubleBuffer::internal_desc() const {
    return m_internal_desc;
}

MemoryPtr VariableStateDoubleBuffer::internal_state_mem() const {
    return prime_mem();
}

VariableStateSingleBuffer::VariableStateSingleBuffer(const std::string& name,
                                                     MemoryPtr external_buffer,
                                                     MemoryDescPtr external_desc)
    : VariableStateBase(name, std::move(external_desc)),
      m_internal_mem(std::move(external_buffer)),
      m_internal_desc(m_internal_mem->getDescPtr()) {
    OPENVINO_ASSERT(m_internal_mem);
    auto&& shape = m_internal_desc->getShape();

    if (shape.isStatic()) {
        m_internal_mem->nullify();
    } else {
        // in the case of the original desc has dynamic shape we create an empty tensor
        auto new_desc = to_static(m_internal_desc);
        m_internal_mem->redefineDesc(new_desc);
    }
}
MemoryPtr VariableStateSingleBuffer::input_mem() {
    return m_internal_mem;
}
MemoryPtr VariableStateSingleBuffer::output_mem() {
    return m_internal_mem;
}
MemoryDescPtr VariableStateSingleBuffer::internal_desc() const {
    return m_internal_desc;
}

void VariableStateSingleBuffer::reset_impl() {
    auto new_desc = to_static(m_internal_desc);
    if (m_internal_mem) {
        m_internal_mem->redefineDesc(new_desc);
        m_internal_mem->nullify();
    }
}

MemoryPtr VariableStateSingleBuffer::internal_state_mem() const {
    return m_internal_mem;
}

void VariableStateSingleBuffer::commit_impl() {
    // nothing to do
}

VariableStateKVcache::VariableStateKVcache(const std::string& name,
                                           MemoryDescPtr external_desc,
                                           BlockedMemoryDescPtr dense_internal_desc,
                                           const bool quant_by_channel,
                                           const size_t group_size)
    : VariableStateBase(name, std::move(external_desc)),
      m_dense_internal_desc(std::move(dense_internal_desc)),
      m_quant_by_channel(quant_by_channel),
      m_group_size(group_size) {
    auto&& shape = get_external_desc()->getShape();
    OPENVINO_ASSERT(shape.isDynamic(), "VariableStateKVcache is unexpectedly initalized with a static tensor");
}

ov::SoPtr<ov::ITensor> VariableStateKVcache::get_state() const {
    if (!m_internal_mem || !m_hidden_state || is_reset_state()) {
        auto new_desc = to_static(get_external_desc());
        auto external_mem = std::make_shared<Memory>(get_engine(), new_desc);
        return std::make_shared<Tensor>(external_mem);
    }

    auto actual_internal_desc = m_internal_mem->getDescWithType<BlockedMemoryDesc>();
    auto&& dims = actual_internal_desc->getShape().getStaticDims();

    auto actual_external_desc = get_external_desc()->cloneWithNewDims(dims);
    auto external_mem = std::make_shared<Memory>(get_engine(), actual_external_desc);

    // let's assume 4th rank KV tensors. This may be extended later
    OPENVINO_ASSERT(actual_internal_desc->getShape().getRank() == 4);
    OPENVINO_ASSERT(actual_external_desc->getShape().getRank() == 4);

    auto&& actual_internal_order = actual_internal_desc->getOrder();
    // sanity check
    OPENVINO_ASSERT(actual_internal_order == m_dense_internal_desc->getOrder());

    PlainTensor output;
    PlainTensor pastkv;
    PlainTensor beam_table;
    output.reset(external_mem);
    beam_table.reset(m_hidden_state);
    pastkv.reset(m_internal_mem);
    output = output.permute(actual_internal_order);
    pastkv = pastkv.permute(actual_internal_order);
    // S should be always the last dimension
    OPENVINO_ASSERT(all_of(1U, pastkv.stride(3), output.stride(3)));
    auto L0 = pastkv.size(0);
    auto B = pastkv.size(1);
    auto H = pastkv.size(2);
    auto S = pastkv.size(3);
    if (pastkv.get_precision() == element::u8) {
        auto nthr = parallel_get_max_threads();
        std::vector<PlainTensor> buffers(nthr);
        if (m_quant_by_channel) {
            parallel_for3d(L0, B, H, [&](size_t ithr, size_t m, size_t b, size_t h) {
                auto b_kv = static_cast<size_t>(beam_table.at<int32_t>({b, m}));
                size_t group_id = m / m_group_size;
                buffers[ithr].resize<float>({S});
                attn_dequant_by_channel_u8(pastkv.ptr<uint8_t>(m, b_kv, h),
                                           buffers[ithr].ptr<float>(),
                                           1,
                                           S,
                                           pastkv.m_strides[2],
                                           S,
                                           m_scale_zp.ptr<float>(group_id * 2, b_kv, h),
                                           m_scale_zp.ptr<float>(group_id * 2 + 1, b_kv, h));
                cpu_convert(buffers[ithr].ptr<float>(), output.ptr_v(m, b, h), element::f32, output.m_dt, S);
            });
        } else {
            parallel_for3d(L0, B, H, [&](size_t ithr, size_t m, size_t b, size_t h) {
                auto b_kv = static_cast<size_t>(beam_table.at<int32_t>({b, m}));
                buffers[ithr].resize<float>({S});
                for (size_t group_id = 0; group_id < S / m_group_size; group_id++) {
                    attn_dequant_u8(pastkv.ptr<uint8_t>(m, b_kv, h, group_id * m_group_size),
                                    buffers[ithr].ptr<float>() + group_id * m_group_size,
                                    m_group_size,
                                    m_scale_zp.ptr<float>(m, b_kv, h, group_id * 2));
                }
                cpu_convert(buffers[ithr].ptr<float>(), output.ptr_v(m, b, h), element::f32, output.m_dt, S);
            });
        }
    } else {
        parallel_for3d(L0, B, H, [&](size_t m, size_t b, size_t h) {
            auto b_kv = static_cast<size_t>(beam_table.at<int32_t>({b, m}));
            cpu_convert(pastkv.ptr_v(m, b_kv, h), output.ptr_v(m, b, h), pastkv.m_dt, output.m_dt, S);
        });
    }

    return std::make_shared<Tensor>(external_mem);
}

void VariableStateKVcache::set_state_impl(const ov::SoPtr<ov::ITensor>& state) {
    // 1. reset the memory object
    m_state = state;  // simply to extend the lifetime
    auto state_desc = MemoryDescUtils::generateCpuBlockedMemoryDesc(m_state);

    // May be optimized by reusing the state tensor underlining memory pointer, but corner cases should be considered
    auto dense_internal_desc = m_dense_internal_desc->cloneWithNewDims(state_desc->getShape().getStaticDims());

    m_internal_mem = std::make_shared<Memory>(get_engine(), dense_internal_desc);
    Memory external_mem(get_engine(), state_desc, m_state->data());

    if (dense_internal_desc->getPrecision() == element::u8) {
        PlainTensor external;
        PlainTensor internal;
        auto&& actual_internal_order = m_dense_internal_desc->getOrder();
        external.resize(external_mem.getStaticDims(),
                        state_desc->getPrecision().size(),
                        state_desc->getPrecision(),
                        m_state->data());
        internal.reset(m_internal_mem);
        external = external.permute(actual_internal_order);
        internal = internal.permute(actual_internal_order);
        auto L0 = internal.size(0);
        auto B = internal.size(1);
        auto H = internal.size(2);
        auto S = internal.size(3);
        auto nthr = parallel_get_max_threads();
        std::vector<PlainTensor> buffers(nthr);
        if (m_quant_by_channel) {
            size_t group_nums = div_up(L0, m_group_size);
            m_scale_zp.resize<float>({group_nums * 2, B, H, S});
            parallel_for3d(group_nums, B, H, [&](size_t ithr, size_t group_id, size_t b, size_t h) {
                size_t valid_seq = std::min(m_group_size, L0 - group_id * m_group_size);
                buffers[ithr].resize<float>({valid_seq, S});
                cpu_convert(external.ptr_v(valid_seq, b, h),
                            buffers[ithr].ptr<float>(),
                            external.m_dt,
                            element::f32,
                            valid_seq * S);
                attn_quant_by_channel_u8(buffers[ithr].ptr<float>(),
                                         internal.ptr<uint8_t>(group_id * m_group_size, b, h),
                                         valid_seq,
                                         S,
                                         S,
                                         internal.m_strides[0],
                                         m_scale_zp.ptr<float>(group_id * 2, b, h),
                                         m_scale_zp.ptr<float>(group_id * 2 + 1, b, h));
            });
        } else {
            m_scale_zp.resize<float>({L0, B, H, 2 * S / m_group_size});
            parallel_for3d(B, H, L0, [&](size_t ithr, size_t b, size_t h, size_t m) {
                buffers[ithr].resize<float>({S});
                cpu_convert(external.ptr_v(m, b, h), buffers[ithr].ptr<float>(), external.m_dt, element::f32, S);
                for (size_t group_id = 0; group_id < S / m_group_size; group_id++) {
                    attn_quant_u8(buffers[ithr].ptr<float>() + group_id * m_group_size,
                                  internal.ptr<uint8_t>(m, b, h, group_id * m_group_size),
                                  m_group_size,
                                  m_scale_zp.at<float>({m, b, h, group_id * 2}),
                                  m_scale_zp.at<float>({m, b, h, group_id * 2 + 1}));
                }
            });
        }
    } else {
        m_internal_mem->load(external_mem, true, false);
    }

    // 2. Reset the beam search table
    auto&& state_dims = dense_internal_desc->getShape().getStaticDims();
    auto&& order = m_dense_internal_desc->getOrder();

    const size_t size_B = state_dims[order.at(1)];
    const size_t size_L = state_dims[order.at(0)];
    auto mem_desc = std::make_shared<CpuBlockedMemoryDesc>(ov::element::i32, Shape{size_B, size_L});

    m_hidden_state = std::make_shared<Memory>(get_engine(), mem_desc);
    auto* buff = m_hidden_state->getDataAs<int>();
    for (size_t i = 0; i < size_B; ++i) {
        for (size_t j = 0; j < size_L; ++j) {
            buff[i * size_L + j] = i;
        }
    }
    m_internal_mem_max_size = dense_internal_desc->getCurrentMemSize() / dense_internal_desc->getPrecision().size();
    m_hidden_state_max_size = mem_desc->getCurrentMemSize() / mem_desc->getPrecision().size();
}

void VariableStateKVcache::reset_impl() {
    // nothing to do
}

void VariableStateKVcache::commit_impl() {
    // nothing to do
}

MemoryPtr VariableStateKVcache::input_mem() {
    return m_internal_mem;
}

MemoryPtr VariableStateKVcache::output_mem() {
    return m_internal_mem;
}

MemoryDescPtr VariableStateKVcache::internal_desc() const {
    return m_dense_internal_desc;  // since we don't store initial one
}

MemoryPtr VariableStateKVcache::internal_state_mem() const {
    return m_internal_mem;
}

void VariableStateKVcache::assign_internal_state(const MemoryPtr& mem) {
    m_internal_mem = mem;
}

MemoryPtr VariableStateKVcache::hidden_state_mem() const {
    return m_hidden_state;
}

void VariableStateKVcache::assign_hidden_state(const MemoryPtr& mem) {
    m_hidden_state = mem;
}
}  // namespace ov::intel_cpu
