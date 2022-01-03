/*
 *    Copyright (C) 2016-2022 Grok Image Compression Inc.
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
 *    This source code incorporates work covered by the BSD 2-clause license.
 *    Please see the LICENSE file in the root directory for details.
 *
 */

#include <cstdio>
#include <cstdlib>
#include "grk_apps_config.h"
#include "grok.h"
#include "TIFFFormat.h"
#include "convert.h"
#include <cstring>
#include "common.h"

#ifndef GROK_HAVE_LIBTIFF
#error GROK_HAVE_LIBTIFF_NOT_DEFINED
#endif /* GROK_HAVE_LIBTIFF */

#include <tiffio.h>
#include <cassert>
#include <memory>
#include <string>

TIFFFormat::TIFFFormat(): tif(nullptr),
							packedBuf(nullptr),
							chroma_subsample_x(1),
							chroma_subsample_y(1),
							rowsWritten(0),
							strip(0),
							rowsPerStrip(0),
							packedBufStride(0),
							units(0),
							bytesToWrite(0)
{
	for(uint32_t i = 0; i < grk::maxNumPackComponents; ++i)
		planes[i] = nullptr;
}
TIFFFormat::~TIFFFormat(){
	if(tif)
		TIFFClose(tif);
	delete[] packedBuf;
}
bool TIFFFormat::encodeHeader(grk_image* image, const std::string& filename,
							  uint32_t compressionParam)
{
	m_image = image;
	m_fileName = filename;

	int tiPhoto = PHOTOMETRIC_MINISBLACK;
	bool success = false;
	int32_t firstExtraChannel = -1;
	uint32_t num_colour_channels = 0;
	size_t numExtraChannels = 0;
	planes[0] = m_image->comps[0].data;
	uint16_t numcomps = m_image->numcomps;
	bool sgnd = m_image->comps[0].sgnd;
	uint32_t width = m_image->comps[0].w;
	uint32_t height = m_image->comps[0].h;
	uint8_t bps = m_image->comps[0].prec;
	bool subsampled = isSubsampled(m_image);

	assert(m_image);
	assert(m_fileName.c_str());

	if(bps == 0)
	{
		spdlog::error("TIFFFormat::encodeHeader: m_image precision is zero.");
		goto cleanup;
	}
	if(!allComponentsSanityCheck(m_image, true))
	{
		spdlog::error("TIFFFormat::encodeHeader: Image sanity check failed.");
		goto cleanup;
	}
	if(m_image->color_space == GRK_CLRSPC_CMYK)
	{
		if(numcomps < 4U)
		{
			spdlog::error(
				"TIFFFormat::encodeHeader: CMYK images shall be composed of at least 4 planes.");

			return false;
		}
		tiPhoto = PHOTOMETRIC_SEPARATED;
		if(numcomps > 4U)
		{
			spdlog::warn("TIFFFormat::encodeHeader: number of components {} is "
						 "greater than 4. Truncating to 4",
						 numcomps);
			numcomps = 4U;
		}
	}
	else if(numcomps > 2U)
	{
		switch(m_image->color_space)
		{
			case GRK_CLRSPC_EYCC:
			case GRK_CLRSPC_SYCC:
				if(subsampled && numcomps != 3)
				{
					spdlog::error("TIFFFormat::encodeHeader: subsampled YCbCr m_image with alpha "
								  "not supported.");
					goto cleanup;
				}
				chroma_subsample_x = m_image->comps[1].dx;
				chroma_subsample_y = m_image->comps[1].dy;
				tiPhoto = PHOTOMETRIC_YCBCR;
				break;
			case GRK_CLRSPC_DEFAULT_CIE:
			case GRK_CLRSPC_CUSTOM_CIE:
				tiPhoto = sgnd ? PHOTOMETRIC_CIELAB : PHOTOMETRIC_ICCLAB;
				break;
			default:
				tiPhoto = PHOTOMETRIC_RGB;
				break;
		}
	}
	if(numcomps > grk::maxNumPackComponents)
	{
		spdlog::error("TIFFFormat::encodeHeader: number of components {} must be <= {}", numcomps,
					  grk::maxNumPackComponents);
		goto cleanup;
	}
	if(isSubsampled(m_image))
	{
		if(tiPhoto != PHOTOMETRIC_YCBCR)
		{
			spdlog::error("TIFFFormat : subsampling only supported for YCbCr images");
			goto cleanup;
		}
		if(!isChromaSubsampled(m_image))
		{
			spdlog::error("TIFFFormat::encodeHeader: only chroma channels can be subsampled");
			goto cleanup;
		}
	}
	// extra channels
	for(uint32_t i = 0U; i < numcomps; ++i)
	{
		if(m_image->comps[i].type != GRK_COMPONENT_TYPE_COLOUR)
		{
			if(firstExtraChannel == -1)
				firstExtraChannel = (int32_t)i;
			numExtraChannels++;
		}
		planes[i] = m_image->comps[i].data;
	}
	// TIFF assumes that alpha channels occur as last channels in m_image.
	if(numExtraChannels > 0)
	{
		num_colour_channels = (uint32_t)(numcomps - (uint32_t)numExtraChannels);
		if((uint32_t)firstExtraChannel < num_colour_channels)
		{
			spdlog::warn(
				"TIFFFormat::encodeHeader: TIFF requires that non-colour channels occur as "
				"last channels in m_image. "
				"TIFFTAG_EXTRASAMPLES tag for extra channels will not be set");
			numExtraChannels = 0;
		}
	}
	tif = TIFFOpen(m_fileName.c_str(), "wb");
	if(!tif)
	{
		spdlog::error("TIFFFormat::encodeHeader:failed to open {} for writing", m_fileName.c_str());
		goto cleanup;
	}
	// calculate rows per strip, base on target 8K strip size
	if(subsampled)
	{
		units = (width + chroma_subsample_x - 1) / chroma_subsample_x;
		packedBufStride = (uint64_t)((((uint64_t)width * chroma_subsample_y + units * 2U) * bps + 7U) / 8U);
		rowsPerStrip = (uint32_t)((chroma_subsample_y * 8 * 1024 * 1024) / packedBufStride);
	}
	else
	{
		units = m_image->comps->w;
		packedBufStride =  grk::PtoI<int32_t>::getPackedBytes(numcomps, width, bps);
		rowsPerStrip = (uint32_t)((16 * 1024 * 1024) / packedBufStride);
	}
	if(rowsPerStrip & 1)
		rowsPerStrip++;
	if(rowsPerStrip > height)
		rowsPerStrip = height;

	TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
	TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
	TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, sgnd ? SAMPLEFORMAT_INT : SAMPLEFORMAT_UINT);
	TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, numcomps);
	TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bps);
	TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
	TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, tiPhoto);
	TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rowsPerStrip);
	if(tiPhoto == PHOTOMETRIC_YCBCR)
	{
		float refBlackWhite[6] = {0.0, 255.0, 128.0, 255.0, 128.0, 255.0};
		float YCbCrCoefficients[3] = {0.299f, 0.587f, 0.114f};

		TIFFSetField(tif, TIFFTAG_YCBCRSUBSAMPLING, chroma_subsample_x, chroma_subsample_y);
		TIFFSetField(tif, TIFFTAG_REFERENCEBLACKWHITE, refBlackWhite);
		TIFFSetField(tif, TIFFTAG_YCBCRCOEFFICIENTS, YCbCrCoefficients);
		TIFFSetField(tif, TIFFTAG_YCBCRPOSITIONING, YCBCRPOSITION_CENTERED);
	}
	switch(compressionParam)
	{
		case COMPRESSION_ADOBE_DEFLATE:
#ifdef ZIP_SUPPORT
			TIFFSetField(tif, TIFFTAG_COMPRESSION, compressionParam); // zip compression
#endif
			break;
		default:
			if(compressionParam != 0)
				TIFFSetField(tif, TIFFTAG_COMPRESSION, compressionParam);
	}
	if(m_image->meta)
	{
		if(m_image->meta->color.icc_profile_buf)
		{
			if(m_image->color_space == GRK_CLRSPC_ICC)
				TIFFSetField(tif, TIFFTAG_ICCPROFILE, m_image->meta->color.icc_profile_len,
							 m_image->meta->color.icc_profile_buf);
		}
		if(m_image->meta->xmp_buf && m_image->meta->xmp_len)
			TIFFSetField(tif, TIFFTAG_XMLPACKET, m_image->meta->xmp_len, m_image->meta->xmp_buf);
		if(m_image->meta->iptc_buf && m_image->meta->iptc_len)
		{
			auto iptc_len = m_image->meta->iptc_len;
			// length must be multiple of 4
			iptc_len += (4 - (iptc_len & 0x03));
			if(iptc_len > m_image->meta->iptc_len)
			{
				auto new_iptf_buf = new uint8_t[iptc_len];
				memset(new_iptf_buf, 0, iptc_len);
				memcpy(new_iptf_buf, m_image->meta->iptc_buf, m_image->meta->iptc_len);
				delete[] m_image->meta->iptc_buf;
				m_image->meta->iptc_buf = new_iptf_buf;
				m_image->meta->iptc_len = iptc_len;
			}
			// Tag is of type TIFF_LONG, so byte length is divided by four
			if(TIFFIsByteSwapped(tif))
				TIFFSwabArrayOfLong((uint32_t*)m_image->meta->iptc_buf,
									(tmsize_t)(m_image->meta->iptc_len / 4));
			TIFFSetField(tif, TIFFTAG_RICHTIFFIPTC, (uint32_t)(m_image->meta->iptc_len / 4),
						 (void*)m_image->meta->iptc_buf);
		}
	}
	if(m_image->capture_resolution[0] > 0 && m_image->capture_resolution[1] > 0)
	{
		TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_CENTIMETER); // cm
		for(int i = 0; i < 2; ++i)
		{
			TIFFSetField(tif, TIFFTAG_XRESOLUTION, m_image->capture_resolution[0] / 100);
			TIFFSetField(tif, TIFFTAG_YRESOLUTION, m_image->capture_resolution[1] / 100);
		}
	}
	if(numExtraChannels)
	{
		std::unique_ptr<uint16_t[]> out(new uint16_t[numExtraChannels]);
		numExtraChannels = 0;
		for(uint32_t i = 0U; i < numcomps; ++i)
		{
			auto comp = m_image->comps + i;
			if(comp->type != GRK_COMPONENT_TYPE_COLOUR)
			{
				if(comp->type == GRK_COMPONENT_TYPE_OPACITY ||
				   comp->type == GRK_COMPONENT_TYPE_PREMULTIPLIED_OPACITY)
					out[numExtraChannels++] = (m_image->comps[i].type == GRK_COMPONENT_TYPE_OPACITY)
												  ? EXTRASAMPLE_UNASSALPHA
												  : EXTRASAMPLE_ASSOCALPHA;
				else
					out[numExtraChannels++] = EXTRASAMPLE_UNSPECIFIED;
			}
		}
		TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, numExtraChannels, out.get());
	}
	if (!m_image->interleavedData)
		packedBuf = new uint8_t[(size_t)TIFFVStripSize(tif, (uint32_t)rowsPerStrip)];
	success = true;
cleanup:
	return success;
}

bool TIFFFormat::writeStrip(void* buf, uint64_t toWrite)
{
	tmsize_t written = TIFFWriteEncodedStrip(tif, (tmsize_t)strip++, buf, (tmsize_t)toWrite);
	if(written == -1)
	{
		spdlog::error("TIFFFormat::encodeRows: error in TIFFWriteEncodedStrip");
		return false;
	}
	if(written != (tmsize_t)toWrite)
	{
		spdlog::error("TIFFFormat::encodeRows: bytes written {} does not equal bytes to write {}",
					  written, toWrite);
		return false;
	}

	return true;
}
bool TIFFFormat::encodeRows(uint32_t rowsToWrite)
{
	bool success = false;
	uint32_t height = m_image->comps[0].h;
	rowsToWrite = (std::min)(rowsToWrite,height - rowsWritten);
	if (rowsToWrite == 0)
		return true;
	uint32_t h = rowsWritten;
	if(isSubsampled(m_image))
	{
		auto bufptr = (int8_t*)packedBuf;
		for(; h < rowsWritten + rowsToWrite; h += chroma_subsample_y)
		{
			uint32_t rowsSoFar = h - rowsWritten;
			if(rowsSoFar > 0 && (rowsSoFar % rowsPerStrip == 0))
			{
				if(!writeStrip(packedBuf, bytesToWrite))
					goto cleanup;
				bufptr = (int8_t*)packedBuf;
				bytesToWrite = 0;
			}
			size_t xpos = 0;
			for(uint32_t u = 0; u < units; ++u)
			{
				// 1. luma
				for(size_t sub_h = 0; sub_h < chroma_subsample_y; ++sub_h)
				{
					for(size_t sub_x = xpos; sub_x < xpos + chroma_subsample_x; ++sub_x)
					{
						bool accept = (h + sub_h) < height && sub_x < m_image->comps[0].w;
						*bufptr++ =
								accept ? (int8_t)planes[0][sub_x + sub_h * m_image->comps[0].stride] : 0;
						bytesToWrite++;
					}
				}
				// 2. chroma
				*bufptr++ = (int8_t)*planes[1]++;
				*bufptr++ = (int8_t)*planes[2]++;
				bytesToWrite += 2;
				xpos += chroma_subsample_x;
			}
			planes[0] += m_image->comps[0].stride * chroma_subsample_y;
			planes[1] += m_image->comps[1].stride - m_image->comps[1].w;
			planes[2] += m_image->comps[2].stride - m_image->comps[2].w;
		}
		if (h != rowsWritten)
			rowsWritten += h - chroma_subsample_y - rowsWritten;
		// cleanup
		if(bytesToWrite && !writeStrip(packedBuf, bytesToWrite))
			goto cleanup;
	}
	else
	{
		tmsize_t hTarget = rowsWritten + rowsToWrite;
		auto iter = grk::InterleaverFactory<int32_t>::makeInterleaver(m_image->comps[0].prec);
		if (!iter)
			goto cleanup;
		auto bufPtr = m_image->interleavedData ? m_image->interleavedData : packedBuf;
		while(h < hTarget)
		{
			uint32_t stripRows = (std::min)(rowsPerStrip, height - h);
			if (!m_image->interleavedData)
				iter->interleave((int32_t**)planes, m_image->numcomps, (uint8_t*)packedBuf, m_image->comps[0].w, m_image->comps[0].stride, packedBufStride, stripRows, 0);
			if(!writeStrip(bufPtr, packedBufStride * stripRows)) {
				delete iter;
				goto cleanup;
			}
			rowsWritten += stripRows;
			h += stripRows;
			if (m_image->interleavedData)
				bufPtr += packedBufStride * stripRows;
		}
		delete iter;
	}
	success = true;
cleanup:
	return success;
}
bool TIFFFormat::encodeFinish(void)
{
	if(tif)
		TIFFClose(tif);
	tif = nullptr;
	delete[] packedBuf;
	packedBuf = nullptr;

	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////

static bool readTiffPixels(TIFF* tif, grk_image_comp* comps, uint32_t numcomps, uint16_t tiSpp,
						   uint16_t tiPC, uint16_t tiPhoto, uint32_t chroma_subsample_x,
						   uint32_t chroma_subsample_y);

static std::string getSampleFormatString(uint16_t tiSampleFormat)
{
	switch(tiSampleFormat)
	{
		case SAMPLEFORMAT_UINT:
			return "UINT";
			break;
		case SAMPLEFORMAT_INT:
			return "INT";
			break;
		case SAMPLEFORMAT_IEEEFP:
			return "IEEEFP";
			break;
		case SAMPLEFORMAT_VOID:
			return "VOID";
			break;
		case SAMPLEFORMAT_COMPLEXINT:
			return "COMPLEXINT";
			break;
		case SAMPLEFORMAT_COMPLEXIEEEFP:
			return "COMPLEXIEEEFP";
			break;
		default:
			return "unknown";
	}
}

static std::string getColourFormatString(uint16_t tiPhoto)
{
	switch(tiPhoto)
	{
		case PHOTOMETRIC_MINISWHITE:
			return "MINISWHITE";
			break;
		case PHOTOMETRIC_MINISBLACK:
			return "MINISBLACK";
			break;
		case PHOTOMETRIC_RGB:
			return "RGB";
			break;
		case PHOTOMETRIC_PALETTE:
			return "PALETTE";
			break;
		case PHOTOMETRIC_MASK:
			return "MASK";
			break;
		case PHOTOMETRIC_SEPARATED:
			return "SEPARATED";
			break;
		case PHOTOMETRIC_YCBCR:
			return "YCBCR";
			break;
		case PHOTOMETRIC_CIELAB:
			return "CIELAB";
			break;
		case PHOTOMETRIC_ICCLAB:
			return "ICCLAB";
			break;
		case PHOTOMETRIC_ITULAB:
			return "ITULAB";
			break;
		case PHOTOMETRIC_CFA:
			return "CFA";
			break;
		case PHOTOMETRIC_LOGL:
			return "LOGL";
			break;
		case PHOTOMETRIC_LOGLUV:
			return "LOGLUV";
			break;
		default:
			return "unknown";
	}
}

static void set_resolution(double* res, float resx, float resy, short resUnit)
{
	// resolution is in pels / metre
	res[0] = resx;
	res[1] = resy;

	switch(resUnit)
	{
		case RESUNIT_INCH:
			// 2.54 cm / inch
			res[0] *= 100 / 2.54;
			res[1] *= 100 / 2.54;
			break;
			// cm
		case RESUNIT_CENTIMETER:
			res[0] *= 100;
			res[1] *= 100;
			break;
		default:
			break;
	}
}

static bool readTiffPixels(TIFF* tif, grk_image_comp* comps, uint32_t numcomps, uint16_t tiSpp,
						   uint16_t tiPC, uint16_t tiPhoto, uint32_t chroma_subsample_x,
						   uint32_t chroma_subsample_y)
{
	if(!tif)
		return false;

	bool success = true;
	cvtTo32 cvtTifTo32s = nullptr;
	cvtInterleavedToPlanar cvtToPlanar = nullptr;
	int32_t* planes[grk::maxNumPackComponents];
	tsize_t rowStride;
	bool invert;
	tdata_t buf = nullptr;
	tstrip_t strip;
	tsize_t strip_size;
	uint32_t currentPlane = 0;
	int32_t* buffer32s = nullptr;
	bool subsampled = chroma_subsample_x != 1 || chroma_subsample_y != 1;
	size_t luma_block = chroma_subsample_x * chroma_subsample_y;
	size_t unitSize = luma_block + 2;

	switch(comps[0].prec)
	{
		case 1:
		case 2:
		case 4:
		case 6:
		case 8:
			cvtTifTo32s = comps[0].sgnd ? cvtsTo32_LUT[comps[0].prec] : cvtTo32_LUT[comps[0].prec];
			break;
			/* others are specific to TIFF */
		case 3:
			cvtTifTo32s = _3uto32s;
			break;
		case 5:
			cvtTifTo32s = _5uto32s;
			break;
		case 7:
			cvtTifTo32s = _7uto32s;
			break;
		case 9:
			cvtTifTo32s = _9uto32s;
			break;
		case 10:
			cvtTifTo32s = comps[0].sgnd ? _10sto32s : _10uto32s;
			break;
		case 11:
			cvtTifTo32s = _11uto32s;
			break;
		case 12:
			cvtTifTo32s = comps[0].sgnd ? _12sto32s : _12uto32s;
			break;
		case 13:
			cvtTifTo32s = _13uto32s;
			break;
		case 14:
			cvtTifTo32s = _14uto32s;
			break;
		case 15:
			cvtTifTo32s = _15uto32s;
			break;
		case 16:
			cvtTifTo32s = (cvtTo32)_16uto32s;
			break;
		default:
			/* never here */
			break;
	}
	cvtToPlanar = cvtInterleavedToPlanar_LUT[numcomps];
	if(tiPC == PLANARCONFIG_SEPARATE)
	{
		cvtToPlanar = cvtInterleavedToPlanar_LUT[1]; /* override */
		tiSpp = 1U; /* consider only one sample per plane */
	}

	strip_size = TIFFStripSize(tif);
	buf = _TIFFmalloc(strip_size);
	if(buf == nullptr)
	{
		success = false;
		goto local_cleanup;
	}
	rowStride = (comps[0].w * tiSpp * comps[0].prec + 7U) / 8U;
	buffer32s = new int32_t[(size_t)comps[0].w * tiSpp];
	strip = 0;
	invert = tiPhoto == PHOTOMETRIC_MINISWHITE;
	for(uint32_t j = 0; j < numcomps; j++)
		planes[j] = comps[j].data;
	do
	{
		auto comp = comps + currentPlane;
		planes[0] = comp->data;
		uint32_t height = 0;
		// if width % chroma_subsample_x != 0...
		size_t units = (comp->w + chroma_subsample_x - 1) / chroma_subsample_x;
		// each coded row will be padded to fill unit
		size_t padding = (units * chroma_subsample_x - comp->w);
		if(subsampled)
			rowStride = (tsize_t)(units * unitSize);
		size_t xpos = 0;
		for(; (height < comp->h) && (strip < TIFFNumberOfStrips(tif)); strip++)
		{
			tsize_t ssize = TIFFReadEncodedStrip(tif, strip, buf, strip_size);
			if(ssize < 1 || ssize > strip_size)
			{
				spdlog::error("tiftoimage: Bad value for ssize({}) "
							  "vs. strip_size({}).",
							  (long long)ssize, (long long)strip_size);
				success = false;
				goto local_cleanup;
			}
			assert(ssize >= rowStride);
			const uint8_t* datau8 = (const uint8_t*)buf;
			while(ssize >= rowStride)
			{
				if(chroma_subsample_x == 1 && chroma_subsample_y == 1)
				{
					cvtTifTo32s(datau8, buffer32s, (size_t)comp->w * tiSpp, invert);
					cvtToPlanar(buffer32s, planes, (size_t)comp->w);
					for(uint32_t k = 0; k < numcomps; ++k)
						planes[k] += comp->stride;
					datau8 += rowStride;
					ssize -= rowStride;
					height++;
				}
				else
				{
					uint32_t strideDiffCb = comps[1].stride - comps[1].w;
					uint32_t strideDiffCr = comps[2].stride - comps[2].w;
					for(size_t i = 0; i < (size_t)rowStride; i += unitSize)
					{
						// process a unit
						// 1. luma
						for(size_t k = 0; k < chroma_subsample_y; ++k)
						{
							for(size_t j = 0; j < chroma_subsample_x; ++j)
							{
								bool accept = height + k < comp->h && xpos + j < comp->w;
								if(accept)
									planes[0][xpos + j + k * comp->stride] = datau8[j];
							}
							datau8 += chroma_subsample_x;
						}
						// 2. chroma
						*planes[1]++ = *datau8++;
						*planes[2]++ = *datau8++;

						// 3. increment raster x
						xpos += chroma_subsample_x;
						if(xpos >= comp->w)
						{
							datau8 += padding;
							xpos = 0;
							planes[0] += comp->stride * chroma_subsample_y;
							planes[1] += strideDiffCb;
							planes[2] += strideDiffCr;
							height += chroma_subsample_y;
						}
					}
					ssize -= rowStride;
				}
			}
		}
		currentPlane++;
	} while((tiPC == PLANARCONFIG_SEPARATE) && (currentPlane < numcomps));
local_cleanup:
	delete[] buffer32s;
	if(buf)
		_TIFFfree(buf);
	return success;
}
template<typename T>
bool readTiffPixelsSigned(TIFF* tif, grk_image_comp* comps, uint32_t numcomps, uint16_t tiSpp,
						  uint16_t tiPC)
{
	if(!tif)
		return false;

	bool success = true;
	cvtInterleavedToPlanar cvtToPlanar = nullptr;
	int32_t* planes[grk::maxNumPackComponents];
	tsize_t rowStride;
	tdata_t buf = nullptr;
	tstrip_t strip;
	tsize_t strip_size;
	uint32_t currentPlane = 0;
	int32_t* buffer32s = nullptr;

	cvtToPlanar = cvtInterleavedToPlanar_LUT[numcomps];
	if(tiPC == PLANARCONFIG_SEPARATE)
	{
		cvtToPlanar = cvtInterleavedToPlanar_LUT[1]; /* override */
		tiSpp = 1U; /* consider only one sample per plane */
	}

	strip_size = TIFFStripSize(tif);
	buf = _TIFFmalloc(strip_size);
	if(buf == nullptr)
	{
		success = false;
		goto local_cleanup;
	}
	rowStride = (comps[0].w * tiSpp * comps[0].prec + 7U) / 8U;
	buffer32s = new int32_t[(size_t)comps[0].w * tiSpp];
	strip = 0;
	for(uint32_t j = 0; j < numcomps; j++)
		planes[j] = comps[j].data;
	do
	{
		grk_image_comp* comp = comps + currentPlane;
		planes[0] = comp->data; /* to manage planar data */
		uint32_t height = comp->h;
		/* Read the Image components */
		for(; (height > 0) && (strip < TIFFNumberOfStrips(tif)); strip++)
		{
			tsize_t ssize = TIFFReadEncodedStrip(tif, strip, buf, strip_size);
			if(ssize < 1 || ssize > strip_size)
			{
				spdlog::error("tiftoimage: Bad value for ssize({}) "
							  "vs. strip_size({}).",
							  (long long)ssize, (long long)strip_size);
				success = false;
				goto local_cleanup;
			}
			const T* data = (const T*)buf;
			while(ssize >= rowStride)
			{
				for(size_t i = 0; i < (size_t)comp->w * tiSpp; ++i)
					buffer32s[i] = data[i];
				cvtToPlanar(buffer32s, planes, (size_t)comp->w);
				for(uint32_t k = 0; k < numcomps; ++k)
					planes[k] += comp->stride;
				data += (size_t)rowStride / sizeof(T);
				ssize -= rowStride;
				height--;
			}
		}
		currentPlane++;
	} while((tiPC == PLANARCONFIG_SEPARATE) && (currentPlane < numcomps));
local_cleanup:
	delete[] buffer32s;
	if(buf)
		_TIFFfree(buf);
	return success;
}

// rec 601 conversion factors, multiplied by 1000
const uint32_t rec_601_luma[3]{299, 587, 114};

grk_image* TIFFFormat::decode(const std::string& filename, grk_cparameters* parameters)
{
	bool found_assocalpha = false;
	size_t alpha_count = 0;
	uint16_t chroma_subsample_x = 1;
	uint16_t chroma_subsample_y = 1;
	GRK_COLOR_SPACE color_space = GRK_CLRSPC_UNKNOWN;
	grk_image_comp cmptparm[grk::maxNumPackComponents];
	grk_image* image = nullptr;
	uint16_t tiBps = 0, tiPhoto = 0, tiSf = SAMPLEFORMAT_UINT, tiSpp = 0, tiPC = 0;
	bool hasTiSf = false;
	short tiResUnit = 0;
	float tiXRes = 0, tiYRes = 0;
	uint32_t tiWidth = 0, tiHeight = 0;
	bool is_cinema = GRK_IS_CINEMA(parameters->rsiz);
	bool success = false;
	bool isCIE = false;
	uint16_t compress;
	float *luma = nullptr, *refBlackWhite = nullptr;
	uint16_t *red_orig = nullptr, *green_orig = nullptr, *blue_orig = nullptr;

	tif = TIFFOpen(filename.c_str(), "r");
	if(!tif)
	{
		spdlog::error("TIFFFormat::decode: Failed to open {} for reading", filename);
		return 0;
	}

	TIFFGetField(tif, TIFFTAG_COMPRESSION, &compress);
	TIFFGetFieldDefaulted(tif, TIFFTAG_IMAGEWIDTH, &tiWidth);
	TIFFGetFieldDefaulted(tif, TIFFTAG_IMAGELENGTH, &tiHeight);
	TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &tiBps);
	TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &tiSpp);
	TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &tiPhoto);
	TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &tiPC);
	hasTiSf = TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &tiSf) == 1;

	TIFFGetFieldDefaulted(tif, TIFFTAG_REFERENCEBLACKWHITE, &refBlackWhite);

	uint32_t w = tiWidth;
	uint32_t h = tiHeight;
	uint16_t numcomps = 0;
	uint32_t icclen = 0;
	uint8_t* iccbuf = nullptr;
	uint8_t* iptc_buf = nullptr;
	uint32_t iptc_len = 0;
	uint8_t* xmp_buf = nullptr;
	uint32_t xmp_len = 0;
	uint16_t* sampleinfo = nullptr;
	uint16_t extrasamples = 0;
	bool hasXRes = false, hasYRes = false, hasResUnit = false;
	bool isSigned = (tiSf == SAMPLEFORMAT_INT);
	bool needSignedPixelReader = isSigned && (tiBps == 8 || tiBps == 16);

	// 1. sanity checks

	// check for supported photometric interpretation
	if(tiPhoto != PHOTOMETRIC_MINISBLACK && tiPhoto != PHOTOMETRIC_MINISWHITE &&
	   tiPhoto != PHOTOMETRIC_RGB && tiPhoto != PHOTOMETRIC_ICCLAB &&
	   tiPhoto != PHOTOMETRIC_CIELAB && tiPhoto != PHOTOMETRIC_YCBCR &&
	   tiPhoto != PHOTOMETRIC_SEPARATED && tiPhoto != PHOTOMETRIC_PALETTE)
	{
		spdlog::error("TIFFFormat::decode: Unsupported color format {}.\n"
					  "Only RGB(A), GRAY(A), CIELAB, YCC, CMYK and PALETTE have been implemented.",
					  getColourFormatString(tiPhoto));
		goto cleanup;
	}
	// check for rec601
	if(tiPhoto == PHOTOMETRIC_YCBCR)
	{
		TIFFGetFieldDefaulted(tif, TIFFTAG_YCBCRCOEFFICIENTS, &luma);
		for(size_t i = 0; i < 3; ++i)
		{
			if((uint32_t)(luma[i] * 1000.0f + 0.5f) != rec_601_luma[i])
			{
				spdlog::error(
					"TIFFFormat::decode: YCbCr image with unsupported non Rec. 601 colour space;");
				spdlog::error("YCbCrCoefficients: {},{},{}", luma[0], luma[1], luma[2]);
				spdlog::error("Please convert to sRGB before compressing.");
				goto cleanup;
			}
		}
	}
	// check sample format
	if(hasTiSf && tiSf != SAMPLEFORMAT_UINT && tiSf != SAMPLEFORMAT_INT)
	{
		spdlog::error("TIFFFormat::decode: Unsupported sample format: {}.",
					  getSampleFormatString(tiSf));
		goto cleanup;
	}
	if(tiSpp == 0)
	{
		spdlog::error("TIFFFormat::decode: Samples per pixel must be non-zero");
		goto cleanup;
	}
	if(tiBps > 16U || tiBps == 0)
	{
		spdlog::error("TIFFFormat::decode: Unsupported precision {}. Maximum 16 Bits supported.",
					  tiBps);
		goto cleanup;
	}
	if(tiWidth == 0 || tiHeight == 0)
	{
		spdlog::error("TIFFFormat::decode: Width({}) and height({}) must both "
					  "be non-zero",
					  tiWidth, tiHeight);
		goto cleanup;
	}
	TIFFGetFieldDefaulted(tif, TIFFTAG_EXTRASAMPLES, &extrasamples, &sampleinfo);

	// 2. initialize image components and signed/unsigned
	memset(&cmptparm[0], 0, grk::maxNumPackComponents * sizeof(grk_image_comp));
	if((tiPhoto == PHOTOMETRIC_RGB) && (is_cinema) && (tiBps != 12U))
	{
		spdlog::warn("TIFFFormat::decode: Input image bitdepth is {} bits\n"
					 "TIF conversion has automatically rescaled to 12-bits\n"
					 "to comply with cinema profiles.",
					 tiBps);
	}
	else
	{
		is_cinema = 0U;
	}
	numcomps = extrasamples;
	switch(tiPhoto)
	{
		case PHOTOMETRIC_PALETTE:
			if(isSigned)
			{
				spdlog::error("TIFFFormat::decode: Signed palette image not supported");
				goto cleanup;
			}
			color_space = GRK_CLRSPC_SRGB;
			numcomps++;
			break;
		case PHOTOMETRIC_MINISBLACK:
		case PHOTOMETRIC_MINISWHITE:
			color_space = GRK_CLRSPC_GRAY;
			numcomps++;
			break;
		case PHOTOMETRIC_RGB:
			color_space = GRK_CLRSPC_SRGB;
			numcomps = (uint16_t)(numcomps + 3);
			break;
		case PHOTOMETRIC_CIELAB:
		case PHOTOMETRIC_ICCLAB:
			isCIE = true;
			color_space = GRK_CLRSPC_DEFAULT_CIE;
			numcomps = (uint16_t)(numcomps + 3);
			break;
		case PHOTOMETRIC_YCBCR:
			// jpeg library is needed to convert from YCbCr to RGB
			if(compress == COMPRESSION_OJPEG || compress == COMPRESSION_JPEG)
			{
				spdlog::error("TIFFFormat::decode: YCbCr image with JPEG compression"
							  " is not supported");
				goto cleanup;
			}
			else if(compress == COMPRESSION_PACKBITS)
			{
				spdlog::error("TIFFFormat::decode: YCbCr image with PACKBITS compression"
							  " is not supported");
				goto cleanup;
			}
			color_space = GRK_CLRSPC_SYCC;
			numcomps = (uint16_t)(numcomps + 3);
			TIFFGetFieldDefaulted(tif, TIFFTAG_YCBCRSUBSAMPLING, &chroma_subsample_x,
								  &chroma_subsample_y);
			if(chroma_subsample_x != 1 || chroma_subsample_y != 1)
			{
				if(isSigned)
				{
					spdlog::error("TIFFFormat::decode: chroma subsampling {},{} with signed data "
								  "is not supported",
								  chroma_subsample_x, chroma_subsample_y);
					goto cleanup;
				}
				if(numcomps != 3)
				{
					spdlog::error("TIFFFormat::decode: chroma subsampling {},{} with alpha "
								  "channel(s) not supported",
								  chroma_subsample_x, chroma_subsample_y);
					goto cleanup;
				}
			}
			break;
		case PHOTOMETRIC_SEPARATED:
			color_space = GRK_CLRSPC_CMYK;
			numcomps = (uint16_t)(numcomps + 4);
			break;
		default:
			spdlog::error("TIFFFormat::decode: Unsupported colour space {}.", tiPhoto);
			goto cleanup;
			break;
	}
	if(tiPhoto == PHOTOMETRIC_CIELAB)
	{
		if(hasTiSf && (tiSf != SAMPLEFORMAT_INT))
			spdlog::warn("TIFFFormat::decode: Input image is in CIE colour space"
						 " but sample format is unsigned int. Forcing to signed int");
		isSigned = true;
	}
	else if(tiPhoto == PHOTOMETRIC_ICCLAB)
	{
		if(hasTiSf && (tiSf != SAMPLEFORMAT_UINT))
			spdlog::warn("TIFFFormat::decode: Input image is in ICC CIE colour"
						 " space but sample format is signed int. Forcing to unsigned int");
		isSigned = false;
	}

	if(isSigned)
	{
		if(tiPhoto == PHOTOMETRIC_MINISWHITE)
			spdlog::error("TIFFFormat::decode: signed image with "
						  "MINISWHITE format is not fully supported");
		if(tiBps != 4 && tiBps != 8 && tiBps != 10 && tiBps != 12 && tiBps != 16)
		{
			spdlog::error("TIFFFormat::decode: signed image with bit"
						  " depth {} is not supported",
						  tiBps);
			goto cleanup;
		}
	}
	if(numcomps > grk::maxNumPackComponents)
	{
		spdlog::error("TIFFFormat::decode: number of components "
					  "{} must be <= %u",
					  numcomps, grk::maxNumPackComponents);
		goto cleanup;
	}

	// 4. create image
	for(uint32_t j = 0; j < numcomps; j++)
	{
		auto img_comp = cmptparm + j;
		img_comp->prec = (uint8_t)tiBps;
		bool chroma = (j == 1 || j == 2);
		img_comp->dx = chroma ? chroma_subsample_x : 1;
		img_comp->dy = chroma ? chroma_subsample_y : 1;
		img_comp->w = grk::ceildiv<uint32_t>(w, img_comp->dx);
		img_comp->h = grk::ceildiv<uint32_t>(h, img_comp->dy);
	}
	image = grk_image_new(numcomps, &cmptparm[0], color_space);
	if(!image)
		goto cleanup;

	/* set image offset and reference grid */
	image->x0 = parameters->image_offset_x0;
	image->x1 = image->x0 + (w - 1) * 1 + 1;
	if(image->x1 <= image->x0)
	{
		spdlog::error("TIFFFormat::decode: Bad value for image->x1({}) vs. "
					  "image->x0({}).",
					  image->x1, image->x0);
		goto cleanup;
	}
	image->y0 = parameters->image_offset_y0;
	image->y1 = image->y0 + (h - 1) * 1 + 1;
	if(image->y1 <= image->y0)
	{
		spdlog::error("TIFFFormat::decode: Bad value for image->y1({}) vs. "
					  "image->y0({}).",
					  image->y1, image->y0);
		goto cleanup;
	}
	if(tiPhoto == PHOTOMETRIC_PALETTE)
	{
		if(!TIFFGetField(tif, TIFFTAG_COLORMAP, &red_orig, &green_orig, &blue_orig))
		{
			spdlog::error("TIFFFormat::decode: Missing required \"Colormap\" tag");
			goto cleanup;
		}
		uint16_t palette_num_entries = (uint16_t)(1U << tiBps);
		uint8_t num_channels = 3U;
		create_meta(image);
		allocPalette(&image->meta->color, num_channels, (uint16_t)palette_num_entries);
		auto cmap = new _grk_component_mapping_comp[num_channels];
		for(uint8_t i = 0; i < num_channels; ++i)
		{
			cmap[i].component_index = 0;
			cmap[i].mapping_type = 1;
			cmap[i].palette_column = i;
			image->meta->color.palette->channel_prec[i] = 16;
			image->meta->color.palette->channel_sign[i] = false;
		}
		image->meta->color.palette->component_mapping = cmap;
		auto lut_ptr = image->meta->color.palette->lut;
		for(uint16_t i = 0; i < palette_num_entries; i++)
		{
			*lut_ptr++ = red_orig[i];
			*lut_ptr++ = green_orig[i];
			*lut_ptr++ = blue_orig[i];
		}
	}
	for(uint32_t j = 0; j < numcomps; j++)
	{
		// handle non-colour channel
		uint16_t numColourChannels = (uint16_t)(numcomps - extrasamples);
		auto comp = image->comps + j;

		if(extrasamples > 0 && j >= numColourChannels)
		{
			comp->type = GRK_COMPONENT_TYPE_UNSPECIFIED;
			comp->association = GRK_COMPONENT_ASSOC_UNASSOCIATED;
			auto alphaType = sampleinfo[j - numColourChannels];
			if(alphaType == EXTRASAMPLE_ASSOCALPHA)
			{
				if(found_assocalpha)
					spdlog::warn(
						"TIFFFormat::decode: Found more than one associated alpha channel");
				alpha_count++;
				comp->type = GRK_COMPONENT_TYPE_PREMULTIPLIED_OPACITY;
				found_assocalpha = true;
			}
			else if(alphaType == EXTRASAMPLE_UNASSALPHA)
			{
				alpha_count++;
				comp->type = GRK_COMPONENT_TYPE_OPACITY;
			}
			else
			{
				// some older mono or RGB images may have alpha channel
				// stored as EXTRASAMPLE_UNSPECIFIED
				if((color_space == GRK_CLRSPC_GRAY && numcomps == 2) ||
				   (color_space == GRK_CLRSPC_SRGB && numcomps == 4))
				{
					alpha_count++;
					comp->type = GRK_COMPONENT_TYPE_OPACITY;
				}
			}
		}
		if(comp->type == GRK_COMPONENT_TYPE_OPACITY ||
		   comp->type == GRK_COMPONENT_TYPE_PREMULTIPLIED_OPACITY)
		{
			switch(alpha_count)
			{
				case 1:
					comp->association = GRK_COMPONENT_ASSOC_WHOLE_IMAGE;
					break;
				case 2:
					comp->association = GRK_COMPONENT_ASSOC_UNASSOCIATED;
					break;
				default:
					comp->type = GRK_COMPONENT_TYPE_UNSPECIFIED;
					comp->association = GRK_COMPONENT_ASSOC_UNASSOCIATED;
					break;
			}
		}
		comp->sgnd = isSigned;
	}

	if(needSignedPixelReader && isSubsampled(image))
	{
		spdlog::error("TIFF: subsampling not supported for signed 8 and 16 bit images");
		goto cleanup;
	}

	// 5. extract capture resolution
	hasXRes = TIFFGetFieldDefaulted(tif, TIFFTAG_XRESOLUTION, &tiXRes) == 1;
	hasYRes = TIFFGetFieldDefaulted(tif, TIFFTAG_YRESOLUTION, &tiYRes) == 1;
	hasResUnit = TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &tiResUnit) == 1;
	if(hasXRes && hasYRes && hasResUnit && tiResUnit != RESUNIT_NONE)
	{
		set_resolution(parameters->capture_resolution_from_file, tiXRes, tiYRes, tiResUnit);
		parameters->write_capture_resolution_from_file = true;
		image->capture_resolution[0] = tiXRes;
		image->capture_resolution[1] = tiYRes;
	}
	// 6. extract embedded ICC profile (with sanity check on binary size of profile)
	// note: we ignore ICC profile for CIE images as JPEG 2000 can't signal both
	// CIE and ICC
	if(!isCIE)
	{
		if((TIFFGetFieldDefaulted(tif, TIFFTAG_ICCPROFILE, &icclen, &iccbuf) == 1) && icclen > 0 &&
		   icclen < grk::maxICCProfileBufferLen)
		{
			if(validate_icc(color_space, iccbuf, icclen))
				copy_icc(image, iccbuf, icclen);
			else
				spdlog::warn("TIFFFormat::decode: ICC profile does not match underlying colour "
							 "space. Ignoring");
		}
	}
	// 7. extract IPTC meta-data
	if(TIFFGetFieldDefaulted(tif, TIFFTAG_RICHTIFFIPTC, &iptc_len, &iptc_buf) == 1)
	{
		if(TIFFIsByteSwapped(tif))
			TIFFSwabArrayOfLong((uint32_t*)iptc_buf, iptc_len);
		// since TIFFTAG_RICHTIFFIPTC is of type TIFF_LONG, we must multiply
		// by 4 to get the length in bytes
		create_meta(image);
		image->meta->iptc_len = iptc_len * 4;
		image->meta->iptc_buf = new uint8_t[iptc_len];
		memcpy(image->meta->iptc_buf, iptc_buf, iptc_len);
	}
	// 8. extract XML meta-data
	if(TIFFGetFieldDefaulted(tif, TIFFTAG_XMLPACKET, &xmp_len, &xmp_buf) == 1)
	{
		create_meta(image);
		image->meta->xmp_len = xmp_len;
		image->meta->xmp_buf = new uint8_t[xmp_len];
		memcpy(image->meta->xmp_buf, xmp_buf, xmp_len);
	}
	// 9. read pixel data
	if(needSignedPixelReader)
	{
		if(tiBps == 8)
			success = readTiffPixelsSigned<int8_t>(tif, image->comps, numcomps, tiSpp, tiPC);
		else
			success = readTiffPixelsSigned<int16_t>(tif, image->comps, numcomps, tiSpp, tiPC);
	}
	else
	{
		success = readTiffPixels(tif, image->comps, numcomps, tiSpp, tiPC, tiPhoto,
								 chroma_subsample_x, chroma_subsample_y);
	}
cleanup:
	if(tif)
		TIFFClose(tif);
	tif = nullptr;
	if(success)
	{
		if(is_cinema)
		{
			for(uint32_t j = 0; j < numcomps; ++j)
				scaleComponent(image->comps+j, 12);
		}
		return image;
	}
	if(image)
		grk_object_unref(&image->obj);

	return nullptr;
}


static void tiff_error(const char* msg, void* client_data)
{
	(void)client_data;
	if(msg)
	{
		std::string out = std::string("libtiff: ") + msg;
		spdlog::error(out);
	}
}
static void tiff_warn(const char* msg, void* client_data)
{
	(void)client_data;
	if(msg)
	{
		std::string out = std::string("libtiff: ") + msg;
		spdlog::warn(out);
	}
}

static bool tiffWarningHandlerVerbose = true;
void MyTiffErrorHandler(const char* module, const char* fmt, va_list ap)
{
	(void)module;
	grk::log(tiff_error, nullptr, fmt, ap);
}

void MyTiffWarningHandler(const char* module, const char* fmt, va_list ap)
{
	(void)module;
	if(tiffWarningHandlerVerbose)
		grk::log(tiff_warn, nullptr, fmt, ap);
}

void tiffSetErrorAndWarningHandlers(bool verbose)
{
	tiffWarningHandlerVerbose = verbose;
	TIFFSetErrorHandler(MyTiffErrorHandler);
	TIFFSetWarningHandler(MyTiffWarningHandler);
}
