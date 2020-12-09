/**
 *    Copyright (C) 2016-2020 Grok Image Compression Inc.
 *
 *    This source code is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This source code is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#pragma once

#include "grk_includes.h"
#include <stdexcept>

namespace grk {

template<typename T> struct res_buf {

	res_buf(Resolution *res, grk_rect_u32 res_bounds) : res(new grk_buffer_2d<T>(res_bounds))
	{
		for (uint32_t i = 0; i < BAND_NUM_INDICES; ++i)
			bandWindow[i] = res ? new grk_buffer_2d<T>(res->bandWindow[i]) : nullptr;
	}
	~res_buf(){
		delete res;
		for (uint32_t i = 0; i < 3; ++i)
			delete bandWindow[i];
	}
	bool alloc(bool clear){
		if (!res->alloc(clear))
			return false;
		for (uint32_t i = 0; i < BAND_NUM_INDICES; ++i){
			if (bandWindow[i] && !bandWindow[i]->alloc(clear))
				return false;
		}
		return true;
	}

	grk_buffer_2d<T> *res;
	grk_buffer_2d<T> *bandWindow[BAND_NUM_INDICES];
};


/*
 Note: various coordinate systems are used to describe regions in the tile buffer.

 1) Canvas coordinate system:  JPEG 2000 global image coordinates, independent of sub-sampling

 2) Tile coordinate system:  coordinates relative to a tile's top left hand corner, with
 sub-sampling accounted for

 3) Resolution coordinate system:  coordinates relative to a resolution's top left hand corner

 4) Sub-band coordinate system: coordinates relative to a particular sub-band's top left hand corner

 */

template<typename T> struct TileComponentWindowBuffer {
	TileComponentWindowBuffer(bool isCompressor,
						bool whole_tile_decoding,
						grk_rect_u32 unreduced_tile_dim,
						grk_rect_u32 reduced_tile_dim,
						grk_rect_u32 unreduced_window_dim,
						Resolution *tile_comp_resolutions,
						uint32_t numresolutions,
						uint32_t reduced_num_resolutions) :
							m_unreduced_bounds(unreduced_tile_dim),
							m_bounds(reduced_tile_dim),
							num_resolutions(numresolutions),
							m_encode(isCompressor),
							whole_tile_decoding(whole_tile_decoding)
	{
		if (!m_encode) {
			m_bounds = unreduced_window_dim.rectceildivpow2(num_resolutions - reduced_num_resolutions);
			m_bounds = m_bounds.intersection(reduced_tile_dim);
			assert(m_bounds.is_valid());

			m_unreduced_bounds = unreduced_window_dim.intersection(unreduced_tile_dim);
			assert(m_unreduced_bounds.is_valid());
		}

		/* fill resolutions vector */
        assert(reduced_num_resolutions>0);

        for (uint32_t resno = 0; resno < reduced_num_resolutions; ++resno)
        	resolutions.push_back(tile_comp_resolutions+resno);

        if ( use_band_buffers()) {
        	// lowest resolution equals 0th band
        	 res_buffers.push_back(new res_buf<T>(nullptr, tile_comp_resolutions->bandWindow[BAND_RES_ZERO_INDEX_LL]) );

        	 for (uint32_t resno = 1; resno < reduced_num_resolutions; ++resno){
        		 auto res_dims =  (resno == num_resolutions - 1) ?  unreduced_window_dim :
															 grk_band_window(num_resolutions,
																				resno+1,
																				0,
																				unreduced_window_dim);
        		 res_buffers.push_back(new res_buf<T>( tile_comp_resolutions+resno, res_dims) );

        	 }
        } else {
        	res_buffers.push_back(new res_buf<T>( nullptr, m_bounds) );
        }
	}
	~TileComponentWindowBuffer(){
		for (auto& b : res_buffers)
			delete b;
	}


	/**
	 * Tranform code block offsets
	 *
	 * @param resno resolution number
	 * @param bandIndex band index (0 for LL band of 0th resolution, otherwise {0,1,2} for {HL,LH,HH} bandWindow
	 * @param offsetx x offset of code block
	 * @param offsety y offset of code block
	 *
	 */
	void transform(uint8_t resno,uint8_t bandIndex, uint32_t &offsetx, uint32_t &offsety) const {
		assert(bandIndex < BAND_NUM_INDICES && resno < resolutions.size());
		if (resno==0)
			assert(bandIndex==BAND_RES_ZERO_INDEX_LL);

		auto res = resolutions[resno];
		auto band = res->bandWindow + bandIndex;

		uint32_t x = offsetx;
		uint32_t y = offsety;

		// get offset relative to band
		x -= band->x0;
		y -= band->y0;

		// if we use one single buffer, then add band offset
		// relative to previous resolution to get correct buffer offset
		if (!use_band_buffers()){
			auto res = (resno == 0) ? nullptr : resolutions[ resno - 1];

			if (band->orientation & 1)
				x += res->width();
			if (band->orientation & 2)
				y += res->height();
		}
		offsetx = x;
		offsety = y;
	}

	/**
	 * Get destination buffer
	 *
	 * @param resno resolution number
	 * @param bandIndex band index (0 for LL band of 0th resolution, otherwise {0,1,2} for {HL,LH,HH} bandWindow
	 *
	 */
	grk_buffer_2d<T>* dest_buf(uint8_t resno,uint8_t bandIndex) const {
		return (use_band_buffers()) ? band_buf(resno,bandIndex) : tile_buf();
	}

	/**
	 * Get pointer to band buffer
	 *
	 * @param resno resolution number
	 * @param bandIndex band index 0 for resno==0 LL band, or {0,1,2} for {HL,LH,HH} bandWindow
	 *
	 */
	T* ptr(uint32_t resno,uint32_t bandIndex) const{
		assert(bandIndex < 3 && resno > 0 && resno < resolutions.size());
		T* rc = nullptr;
		if (use_band_buffers()){
			rc =  band_buf(resno,bandIndex)->data;
		} else {
			auto lower_res = resolutions[resno-1];
			switch(bandIndex){
			case 0:
				rc = tile_buf()->data + lower_res->width();
				break;
			case 1:
				rc = tile_buf()->data + lower_res->height() * stride(resno,bandIndex);
				break;
			case 2:
				rc = tile_buf()->data + lower_res->width() +
						lower_res->height() * stride(resno,bandIndex);
				break;
			default:
				break;
			}
		}

		return rc;
	}

	/**
	 * Get pointer to resolution buffer
	 *
	 * @param resno resolution number
	 *
	 */
	T* ptr(uint32_t resno) const{
		return (use_band_buffers()) ? res_buffers[resno]->res->data : tile_buf()->data;
	}

	/**
	 * Get pointer to tile buffer
	 *
	 *
	 */
	T* ptr(void) const{
		return tile_buf()->data;
	}
	/**
	 * Get stride of band buffer
	 *
	 * @param resno resolution number
	 * @param bandIndex band index 0 for resno==0 LL band, or {0,1,2} for {HL,LH,HH} bandWindow
	 */
	uint32_t stride(uint32_t resno,uint32_t bandIndex) const{
		assert(bandIndex < 3 && resno < resolutions.size());

		return (use_band_buffers()) ? band_buf(resno,bandIndex)->stride : tile_buf()->stride;
	}

	uint32_t stride(uint32_t resno) const{
		return (use_band_buffers()) ? res_buffers[resno]->res->stride : tile_buf()->stride;
	}

	uint32_t stride(void) const{
		return tile_buf()->stride;
	}


	bool alloc(){
		for (auto& b : res_buffers) {
			if (!b->alloc(!m_encode))
				return false;
		}
		// sanity check
		for (uint32_t i = 1; i < res_buffers.size(); ++i){
			auto b = res_buffers[i];
			auto b_prev = res_buffers[i-1];
			if (!b_prev->res->data)
				b_prev->res->data = b->bandWindow[0]->data;
			if (!b->bandWindow[1]->data)
				b->bandWindow[1]->data = b->bandWindow[2]->data;
		}
		return true;
	}

	/**
	 * Get bounds of tile component
	 * decompress: reduced tile component coordinates of window
	 * compress: unreduced tile component coordinates of entire tile
	 */
	grk_rect_u32 bounds() const{
		return m_bounds;
	}

	grk_rect_u32 unreduced_bounds() const{
		return m_unreduced_bounds;
	}

	uint64_t strided_area(void){
		return stride() * m_bounds.height();
	}

	// set data to buf without owning it
	void attach(T* buffer,uint32_t stride){
		tile_buf()->attach(buffer,stride);
	}
	// set data to buf and own it
	void acquire(T* buffer, uint32_t stride){
		tile_buf()->acquire(buffer,stride);
	}
	// transfer data to buf, and cease owning it
	void transfer(T** buffer, bool* owns, uint32_t *stride){
		tile_buf()->transfer(buffer,owns,stride);
	}

private:

	bool use_band_buffers() const{
		//return !m_encode && whole_tile_decoding && resolutions.size() > 1;
		return false;
	}

	grk_buffer_2d<T>* band_buf(uint32_t resno,uint32_t bandIndex) const{
		assert(bandIndex < 3 && resno < resolutions.size());

		return resno > 0 ? res_buffers[resno]->bandWindow[bandIndex] : res_buffers[resno]->res;
	}

	grk_buffer_2d<T>* tile_buf() const{
		return res_buffers.back()->res;
	}

	grk_rect_u32 m_unreduced_bounds;

	/* decompress: reduced tile component coordinates of window  */
	/* compress: unreduced tile component coordinates of entire tile */
	grk_rect_u32 m_bounds;

	std::vector<Resolution*> resolutions;
	std::vector<res_buf<T>* > res_buffers;
	uint32_t num_resolutions;

	bool m_encode;
	bool whole_tile_decoding;
};


}