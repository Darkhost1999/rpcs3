#include "stdafx.h"
#include "RSXDrawCommands.h"

#include "Emu/RSX/Common/BufferUtils.h"
#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/RSXThread.h"

#include "Emu/Memory/vm.h"

namespace rsx
{
	void draw_command_processor::analyse_inputs_interleaved(vertex_input_layout& result, const vertex_program_metadata_t& vp_metadata)
	{
		const rsx_state& state = rsx::method_registers;
		const u32 input_mask = state.vertex_attrib_input_mask() & vp_metadata.referenced_inputs_mask;

		result.clear();
		result.attribute_mask = static_cast<u16>(input_mask);

		if (state.current_draw_clause.command == rsx::draw_command::inlined_array)
		{
			interleaved_range_info& info = *result.alloc_interleaved_block();
			info.interleaved = true;

			for (u8 index = 0; index < rsx::limits::vertex_count; ++index)
			{
				auto& vinfo = state.vertex_arrays_info[index];
				result.attribute_placement[index] = attribute_buffer_placement::none;

				if (vinfo.size() > 0)
				{
					// Stride must be updated even if the stream is disabled
					info.attribute_stride += rsx::get_vertex_type_size_on_host(vinfo.type(), vinfo.size());
					info.locations.push_back({ index, false, 1 });

					if (input_mask & (1u << index))
					{
						result.attribute_placement[index] = attribute_buffer_placement::transient;
					}
				}
				else if (state.register_vertex_info[index].size > 0 && input_mask & (1u << index))
				{
					// Reads from register
					result.referenced_registers.push_back(index);
					result.attribute_placement[index] = attribute_buffer_placement::transient;
				}
			}

			if (info.attribute_stride)
			{
				// At least one array feed must be enabled for vertex input
				result.interleaved_blocks.push_back(&info);
			}

			return;
		}

		const u32 frequency_divider_mask = rsx::method_registers.frequency_divider_operation_mask();
		result.interleaved_blocks.reserve(16);
		result.referenced_registers.reserve(16);

		for (auto [ref_mask, index] = std::tuple{ input_mask, u8(0) }; ref_mask; ++index, ref_mask >>= 1)
		{
			ensure(index < rsx::limits::vertex_count);

			if (!(ref_mask & 1u))
			{
				// Nothing to do, uninitialized
				continue;
			}

			// Always reset attribute placement by default
			result.attribute_placement[index] = attribute_buffer_placement::none;

			// Check for interleaving
			if (rsx::method_registers.current_draw_clause.is_immediate_draw &&
				rsx::method_registers.current_draw_clause.command != rsx::draw_command::indexed)
			{
				// NOTE: In immediate rendering mode, all vertex setup is ignored
				// Observed with GT5, immediate render bypasses array pointers completely, even falling back to fixed-function register defaults
				if (m_vertex_push_buffers[index].vertex_count > 1)
				{
					// Ensure consistent number of vertices per attribute.
					m_vertex_push_buffers[index].pad_to(m_vertex_push_buffers[0].vertex_count, false);

					// Read temp buffer (register array)
					std::pair<u8, u32> volatile_range_info = std::make_pair(index, static_cast<u32>(m_vertex_push_buffers[index].data.size() * sizeof(u32)));
					result.volatile_blocks.push_back(volatile_range_info);
					result.attribute_placement[index] = attribute_buffer_placement::transient;
				}
				else if (state.register_vertex_info[index].size > 0)
				{
					// Reads from register
					result.referenced_registers.push_back(index);
					result.attribute_placement[index] = attribute_buffer_placement::transient;
				}

				// Fall back to the default register value if no source is specified via register
				continue;
			}

			const auto& info = state.vertex_arrays_info[index];
			if (!info.size())
			{
				if (state.register_vertex_info[index].size > 0)
				{
					//Reads from register
					result.referenced_registers.push_back(index);
					result.attribute_placement[index] = attribute_buffer_placement::transient;
					continue;
				}
			}
			else
			{
				result.attribute_placement[index] = attribute_buffer_placement::persistent;
				const u32 base_address = info.offset() & 0x7fffffff;
				bool alloc_new_block = true;
				bool modulo = !!(frequency_divider_mask & (1 << index));

				for (auto& block : result.interleaved_blocks)
				{
					if (block->single_vertex)
					{
						//Single vertex definition, continue
						continue;
					}

					if (block->attribute_stride != info.stride())
					{
						//Stride does not match, continue
						continue;
					}

					if (base_address > block->base_offset)
					{
						const u32 diff = base_address - block->base_offset;
						if (diff > info.stride())
						{
							//Not interleaved, continue
							continue;
						}
					}
					else
					{
						const u32 diff = block->base_offset - base_address;
						if (diff > info.stride())
						{
							//Not interleaved, continue
							continue;
						}

						//Matches, and this address is lower than existing
						block->base_offset = base_address;
					}

					alloc_new_block = false;
					block->locations.push_back({ index, modulo, info.frequency() });
					block->interleaved = true;
					break;
				}

				if (alloc_new_block)
				{
					interleaved_range_info& block = *result.alloc_interleaved_block();
					block.base_offset = base_address;
					block.attribute_stride = info.stride();
					block.memory_location = info.offset() >> 31;
					block.locations.reserve(16);
					block.locations.push_back({ index, modulo, info.frequency() });

					if (block.attribute_stride == 0)
					{
						block.single_vertex = true;
						block.attribute_stride = rsx::get_vertex_type_size_on_host(info.type(), info.size());
					}

					result.interleaved_blocks.push_back(&block);
				}
			}
		}

		for (auto& info : result.interleaved_blocks)
		{
			//Calculate real data address to be used during upload
			info->real_offset_address = rsx::get_address(rsx::get_vertex_offset_from_base(state.vertex_data_base_offset(), info->base_offset), info->memory_location);
		}
	}

	std::span<const std::byte> draw_command_processor::get_raw_index_array(const draw_clause& draw_indexed_clause) const
	{
		if (!m_element_push_buffer.empty()) [[ unlikely ]]
		{
			// Indices provided via immediate mode
			return { reinterpret_cast<const std::byte*>(m_element_push_buffer.data()), ::narrow<u32>(m_element_push_buffer.size() * sizeof(u32)) };
		}

		const rsx::index_array_type type = rsx::method_registers.index_type();
		const u32 type_size = get_index_type_size(type);

		// Force aligned indices as realhw
		const u32 address = (0 - type_size) & get_address(rsx::method_registers.index_array_address(), rsx::method_registers.index_array_location());

		const u32 first = draw_indexed_clause.min_index();
		const u32 count = draw_indexed_clause.get_elements_count();

		const auto ptr = vm::_ptr<const std::byte>(address);
		return { ptr + first * type_size, count * type_size };
	}

	std::variant<draw_array_command, draw_indexed_array_command, draw_inlined_array>
		draw_command_processor::get_draw_command(const rsx::rsx_state& state) const
	{
		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::indexed) [[ likely ]]
		{
			return draw_indexed_array_command
			{
				get_raw_index_array(state.current_draw_clause)
			};
		}

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::array)
		{
			return draw_array_command{};
		}

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array)
		{
			return draw_inlined_array{};
		}

		fmt::throw_exception("ill-formed draw command");
	}

	void draw_command_processor::append_to_push_buffer(u32 attribute, u32 size, u32 subreg_index, vertex_base_type type, u32 value)
	{
		if (!(rsx::method_registers.vertex_attrib_input_mask() & (1 << attribute)))
		{
			return;
		}

		// Enforce ATTR0 as vertex attribute for push buffers.
		// This whole thing becomes a mess if we don't have a provoking attribute.
		const auto vertex_id = m_vertex_push_buffers[0].get_vertex_id();
		m_vertex_push_buffers[attribute].set_vertex_data(attribute, vertex_id, subreg_index, type, size, value);
		m_thread->m_graphics_state |= rsx::pipeline_state::push_buffer_arrays_dirty;
	}

	u32 draw_command_processor::get_push_buffer_vertex_count() const
	{
		// Enforce ATTR0 as vertex attribute for push buffers.
		// This whole thing becomes a mess if we don't have a provoking attribute.
		return m_vertex_push_buffers[0].vertex_count;
	}

	void draw_command_processor::append_array_element(u32 index)
	{
		// Endianness is swapped because common upload code expects input in BE
		// TODO: Implement fast upload path for LE inputs and do away with this
		m_element_push_buffer.push_back(std::bit_cast<u32, be_t<u32>>(index));
	}

	u32 draw_command_processor::get_push_buffer_index_count() const
	{
		return ::size32(m_element_push_buffer);
	}

	void draw_command_processor::clear_push_buffers()
	{
		auto& graphics_state = m_thread->m_graphics_state;
		if (graphics_state & rsx::pipeline_state::push_buffer_arrays_dirty)
		{
			for (auto& push_buf : m_vertex_push_buffers)
			{
				//Disabled, see https://github.com/RPCS3/rpcs3/issues/1932
				//rsx::method_registers.register_vertex_info[index].size = 0;

				push_buf.clear();
			}

			graphics_state.clear(rsx::pipeline_state::push_buffer_arrays_dirty);
		}

		m_element_push_buffer.clear();
	}

	void draw_command_processor::fill_vertex_layout_state(
		const vertex_input_layout& layout,
		const vertex_program_metadata_t& vp_metadata,
		u32 first_vertex,
		u32 vertex_count,
		s32* buffer,
		u32 persistent_offset_base,
		u32 volatile_offset_base) const
	{
		std::array<s32, 16> offset_in_block = {};
		u32 volatile_offset = volatile_offset_base;
		u32 persistent_offset = persistent_offset_base;

		//NOTE: Order is important! Transient ayout is always push_buffers followed by register data
		if (rsx::method_registers.current_draw_clause.is_immediate_draw)
		{
			for (const auto& info : layout.volatile_blocks)
			{
				offset_in_block[info.first] = volatile_offset;
				volatile_offset += info.second;
			}
		}

		for (u8 index : layout.referenced_registers)
		{
			offset_in_block[index] = volatile_offset;
			volatile_offset += 16;
		}

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array)
		{
			const auto& block = layout.interleaved_blocks[0];
			u32 inline_data_offset = volatile_offset;
			for (const auto& attrib : block->locations)
			{
				auto& info = rsx::method_registers.vertex_arrays_info[attrib.index];

				offset_in_block[attrib.index] = inline_data_offset;
				inline_data_offset += rsx::get_vertex_type_size_on_host(info.type(), info.size());
			}
		}
		else
		{
			for (const auto& block : layout.interleaved_blocks)
			{
				for (const auto& attrib : block->locations)
				{
					const u32 local_address = (rsx::method_registers.vertex_arrays_info[attrib.index].offset() & 0x7fffffff);
					offset_in_block[attrib.index] = persistent_offset + (local_address - block->base_offset);
				}

				const auto range = block->calculate_required_range(first_vertex, vertex_count);
				persistent_offset += block->attribute_stride * range.second;
			}
		}

		// Fill the data
		// Each descriptor field is 64 bits wide
		// [0-8] attribute stride
		// [8-24] attribute divisor
		// [24-27] attribute type
		// [27-30] attribute size
		// [30-31] reserved
		// [31-60] starting offset
		// [60-21] swap bytes flag
		// [61-22] volatile flag
		// [62-63] modulo enable flag

		const s32 default_frequency_mask = (1 << 8);
		const s32 swap_storage_mask = (1 << 29);
		const s32 volatile_storage_mask = (1 << 30);
		const s32 modulo_op_frequency_mask = smin;

		const u32 modulo_mask = rsx::method_registers.frequency_divider_operation_mask();
		const auto max_index = (first_vertex + vertex_count) - 1;

		for (u16 ref_mask = vp_metadata.referenced_inputs_mask, index = 0; ref_mask; ++index, ref_mask >>= 1)
		{
			if (!(ref_mask & 1u))
			{
				// Unused input, ignore this
				continue;
			}

			if (layout.attribute_placement[index] == attribute_buffer_placement::none)
			{
				static constexpr u64 zero = 0;
				std::memcpy(buffer + index * 2, &zero, sizeof(zero));
				continue;
			}

			rsx::vertex_base_type type = {};
			s32 size = 0;
			s32 attrib0 = 0;
			s32 attrib1 = 0;

			if (layout.attribute_placement[index] == attribute_buffer_placement::transient)
			{
				if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array)
				{
					const auto& info = rsx::method_registers.vertex_arrays_info[index];

					if (!info.size())
					{
						// Register
						const auto& reginfo = rsx::method_registers.register_vertex_info[index];
						type = reginfo.type;
						size = reginfo.size;

						attrib0 = rsx::get_vertex_type_size_on_host(type, size);
					}
					else
					{
						// Array
						type = info.type();
						size = info.size();

						attrib0 = layout.interleaved_blocks[0]->attribute_stride | default_frequency_mask;
					}
				}
				else
				{
					// Data is either from an immediate render or register input
					// Immediate data overrides register input

					if (rsx::method_registers.current_draw_clause.is_immediate_draw &&
						m_vertex_push_buffers[index].vertex_count > 1)
					{
						// Push buffer
						const auto& info = m_vertex_push_buffers[index];
						type = info.type;
						size = info.size;

						attrib0 = rsx::get_vertex_type_size_on_host(type, size) | default_frequency_mask;
					}
					else
					{
						// Register
						const auto& info = rsx::method_registers.register_vertex_info[index];
						type = info.type;
						size = info.size;

						attrib0 = rsx::get_vertex_type_size_on_host(type, size);
					}
				}

				attrib1 |= volatile_storage_mask;
			}
			else
			{
				auto& info = rsx::method_registers.vertex_arrays_info[index];
				type = info.type();
				size = info.size();

				auto stride = info.stride();
				attrib0 = stride;

				if (stride > 0) //when stride is 0, input is not an array but a single element
				{
					const u32 frequency = info.frequency();
					switch (frequency)
					{
					case 0:
					case 1:
					{
						attrib0 |= default_frequency_mask;
						break;
					}
					default:
					{
						if (modulo_mask & (1 << index))
						{
							if (max_index >= frequency)
							{
								// Only set modulo mask if a modulo op is actually necessary!
								// This requires that the uploaded range for this attr = [0, freq-1]
								// Ignoring modulo op if the rendered range does not wrap allows for range optimization
								attrib0 |= (frequency << 8);
								attrib1 |= modulo_op_frequency_mask;
							}
							else
							{
								attrib0 |= default_frequency_mask;
							}
						}
						else
						{
							// Division
							attrib0 |= (frequency << 8);
						}
						break;
					}
					}
				}
			} //end attribute placement check

			// Special compressed 4 components into one 4-byte value. Decoded as one value.
			if (type == rsx::vertex_base_type::cmp)
			{
				size = 1;
			}

			// All data is passed in in PS3-native order (BE) so swap flag should be set
			attrib1 |= swap_storage_mask;
			attrib0 |= (static_cast<s32>(type) << 24);
			attrib0 |= (size << 27);
			attrib1 |= offset_in_block[index];

			buffer[index * 2 + 0] = attrib0;
			buffer[index * 2 + 1] = attrib1;
		}
	}

	void draw_command_processor::write_vertex_data_to_memory(
		const vertex_input_layout& layout,
		u32 first_vertex,
		u32 vertex_count,
		void* persistent_data,
		void* volatile_data) const
	{
		auto transient = static_cast<char*>(volatile_data);
		auto persistent = static_cast<char*>(persistent_data);

		auto& draw_call = rsx::method_registers.current_draw_clause;

		if (transient != nullptr)
		{
			if (draw_call.command == rsx::draw_command::inlined_array)
			{
				for (const u8 index : layout.referenced_registers)
				{
					memcpy(transient, rsx::method_registers.register_vertex_info[index].data.data(), 16);
					transient += 16;
				}

				memcpy(transient, draw_call.inline_vertex_array.data(), draw_call.inline_vertex_array.size() * sizeof(u32));
				//Is it possible to reference data outside of the inlined array?
				return;
			}

			//NOTE: Order is important! Transient layout is always push_buffers followed by register data
			if (draw_call.is_immediate_draw)
			{
				//NOTE: It is possible for immediate draw to only contain index data, so vertex data can be in persistent memory
				for (const auto& info : layout.volatile_blocks)
				{
					memcpy(transient, m_vertex_push_buffers[info.first].data.data(), info.second);
					transient += info.second;
				}
			}

			for (const u8 index : layout.referenced_registers)
			{
				memcpy(transient, rsx::method_registers.register_vertex_info[index].data.data(), 16);
				transient += 16;
			}
		}

		if (persistent != nullptr)
		{
			for (interleaved_range_info* block : layout.interleaved_blocks)
			{
				auto range = block->calculate_required_range(first_vertex, vertex_count);

				const u32 data_size = range.second * block->attribute_stride;
				const u32 vertex_base = range.first * block->attribute_stride;

				g_fxo->get<rsx::dma_manager>().copy(persistent, vm::_ptr<char>(block->real_offset_address) + vertex_base, data_size);
				persistent += data_size;
			}
		}
	}
}