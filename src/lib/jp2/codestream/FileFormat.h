/*
 *    Copyright (C) 2016-2021 Grok Image Compression Inc.
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
 *
 *    This source code incorporates work covered by the BSD 2-clause license.
 *    Please see the LICENSE file in the root directory for details.
 *
 */


#pragma once

#include <string>

namespace grk {

/**
 @file FileFormat.h
 @brief The JPEG 2000 file format Reader/Writer (JP2)

 */

/** @defgroup JP2 JP2 - JPEG 2000 file format reader/writer */
/*@{*/

#define     JP2_JP   0x6a502020    /**< JPEG 2000 signature box */
#define     JP2_FTYP 0x66747970    /**< File type box */
#define     JP2_JP2H 0x6a703268    /**< JP2 header box (super-box) */
#define     JP2_IHDR 0x69686472    /**< Image header box */
#define     JP2_COLR 0x636f6c72    /**< Colour specification box */
#define     JP2_JP2C 0x6a703263    /**< Contiguous code stream box */
#define     JP2_PCLR 0x70636c72    /**< Palette box */
#define     JP2_CMAP 0x636d6170    /**< Component Mapping box */
#define     JP2_CDEF 0x63646566    /**< Channel Definition box */
#define     JP2_DTBL 0x6474626c    /**< Data Reference box */
#define     JP2_BPCC 0x62706363    /**< Bits per component box */
#define     JP2_JP2  0x6a703220    /**< File type fields */

#define JP2_RES			0x72657320   /**< Resolution box (super-box) */
#define JP2_CAPTURE_RES 0x72657363   /**< Capture resolution box */
#define JP2_DISPLAY_RES 0x72657364   /**< Display resolution box */

#define JP2_JP2I 0x6a703269   /**< Intellectual property box */
#define JP2_XML  0x786d6c20   /**< XML box */
#define JP2_UUID 0x75756964   /**< UUID box */
#define JP2_UINF 0x75696e66   /**< UUID info box (super-box) */
#define JP2_ULST 0x756c7374   /**< UUID list box */
#define JP2_URL  0x75726c20   /**< Data entry URL box */
#define JP2_ASOC 0x61736f63   /**< Associated data */
#define JP2_LBL  0x6c626c20   /**< Association label */


/* ----------------------------------------------------------------------- */

#define JP2_MAX_NUM_UUIDS	128

const uint8_t IPTC_UUID[16] = { 0x33, 0xC7, 0xA4, 0xD2, 0xB8, 0x1D, 0x47, 0x23,
		0xA0, 0xBA, 0xF1, 0xA3, 0xE0, 0x97, 0xAD, 0x38 };
const uint8_t XMP_UUID[16] = { 0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8,
		0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC };

enum JP2_STATE {
	JP2_STATE_NONE = 0x0,
	JP2_STATE_SIGNATURE = 0x1,
	JP2_STATE_FILE_TYPE = 0x2,
	JP2_STATE_HEADER = 0x4,
	JP2_STATE_CODESTREAM = 0x8,
	JP2_STATE_END_CODESTREAM = 0x10,
	JP2_STATE_UNKNOWN = 0x7fffffff /* ISO C restricts enumerator values to range of 'int' */
};

enum JP2_IMG_STATE {
	JP2_IMG_STATE_NONE = 0x0, JP2_IMG_STATE_UNKNOWN = 0x7fffffff
};

/**
 JP2 component
 */
struct grk_jp2_comps {
	grk_jp2_comps() : bpc(0)
	{}
	uint8_t bpc;
};

struct grk_jp2_buffer {
	grk_jp2_buffer(uint8_t *buf, size_t size, bool owns) :
			buffer(buf), len(size), ownsData(owns) {
	}
	grk_jp2_buffer() :
			grk_jp2_buffer(nullptr, 0, false) {
	}
	virtual ~grk_jp2_buffer(){}
	void alloc(size_t length) {
		dealloc();
		buffer = new uint8_t[length];
		len = length;
		ownsData = true;
	}
	virtual void dealloc() {
		if (ownsData)
			delete[] buffer;
		buffer = nullptr;
		ownsData = false;
		len = 0;
	}
	uint8_t *buffer;
	size_t len;
	bool ownsData;
};

/**
	Association box (ASOC data) struct, defined by level, label and optionally XML data.
	See here: http://docs.opengeospatial.org/is/08-085r4/08-085r4.html for GML specification
	The first GML asoc is named 'gml.data' and has no XML data.
	The second GML asoc is named 'gml.root-instance'
	and contains XML formatted geo-information.
*/
struct grk_jp2_asoc : grk_jp2_buffer{
	virtual ~grk_jp2_asoc() override {
		dealloc();
	}
	void dealloc() override {
		grk_jp2_buffer::dealloc();
		for (auto& as : children){
			delete as;
		}
		children.clear();
	}
    std::string label;
    std::vector<grk_jp2_asoc*> children;
};


struct grk_jp2_uuid: public grk_jp2_buffer {
	grk_jp2_uuid() : grk_jp2_buffer() {}
	grk_jp2_uuid(const uint8_t myuuid[16], uint8_t *buf, size_t size, bool owns) :
			grk_jp2_buffer(buf, size, owns) {
		for (int i = 0; i < 16; ++i)
			uuid[i] = myuuid[i];
	}
	uint8_t uuid[16];
};

/**
 JP2 Box
 */
struct grk_jp2_box {
	uint64_t length;
	uint32_t type;
};

class FileFormat;

struct grk_jp2_header_handler {
	/* marker value */
	uint32_t id;
	/* action linked to the marker */
	bool (*handler)(FileFormat *fileFormat, uint8_t *p_header_data, uint32_t header_size);
};

struct grk_jp2_img_header_writer_handler {
	/* action to perform */
	uint8_t* (*handler)(FileFormat *fileFormat, uint32_t *data_size);
	/* result of the action : data */
	uint8_t *m_data;
	/* size of data */
	uint32_t m_size;
};

struct FileFormat;
typedef bool (*jp2_procedure)(FileFormat *fileFormat);

/**
 JPEG 2000 file format reader/writer
 */
struct FileFormat : public ICodeStream {
	FileFormat(bool isDecoder, BufferedStream *stream);
	~FileFormat();


	/** Main header reading function handler */
   bool read_header(grk_header_info  *header_info, grk_image **p_image);

	/** Set up decompressor function handler */
  void init_decompress(grk_dparameters  *p_param);

	/**
	 * Sets the given area to be decompressed. This function should be called right after grk_read_header
	 * and before any tile header reading.
	 *
	 * @param	p_image     image
	 * @param	start_x		the left position of the rectangle to decompress (in image coordinates).
	 * @param	start_y		the up position of the rectangle to decompress (in image coordinates).
	 * @param	end_x		the right position of the rectangle to decompress (in image coordinates).
	 * @param	end_y		the bottom position of the rectangle to decompress (in image coordinates).

	 *
	 * @return	true			if the area could be set.
	 */
	bool set_decompress_window(grk_image *p_image,
						uint32_t start_x,
						uint32_t start_y,
						uint32_t end_x,
						uint32_t end_y);


   bool decompress( grk_plugin_tile *tile,	grk_image *p_image);

   bool end_decompress(void);

   bool init_compress(grk_cparameters  *p_param,grk_image *p_image);

   bool start_compress(void);

   bool compress(grk_plugin_tile* tile);

   bool compress_tile(uint16_t tile_index,	uint8_t *p_data, uint64_t data_size);

   bool end_compress(void);

	bool decompress_tile(grk_image *p_image,uint16_t tile_index);

   void dump(uint32_t flag, FILE *out_stream);

   grk_codestream_info_v2* get_cstr_info(void);

   grk_codestream_index* get_cstr_index(void);

   static void free_color(grk_jp2_color *color);
   static void alloc_palette(grk_jp2_color *color, uint8_t num_channels, uint16_t num_entries);
   static void free_palette_clr(grk_jp2_color *color);

   uint32_t read_asoc(grk_jp2_asoc *parent,
		   	   	   	   uint8_t **header_data,
					   uint32_t *header_data_size,
					   uint32_t asocSize);
   bool read_header_procedure(void);
   bool default_validation(void);
   bool read_box_hdr(grk_jp2_box *box, uint32_t *p_number_bytes_read,BufferedStream *stream);
   bool read_ihdr( uint8_t *p_image_header_data,uint32_t image_header_size);
   uint8_t* write_ihdr( uint32_t *p_nb_bytes_written);
   uint8_t* write_buffer(uint32_t boxId, grk_jp2_buffer *buffer,uint32_t *p_nb_bytes_written);
   bool read_xml( uint8_t *p_xml_data, uint32_t xml_size);
   uint8_t* write_xml( uint32_t *p_nb_bytes_written);
   bool read_uuid( uint8_t *p_header_data,uint32_t header_size);

   double calc_res(uint16_t num, uint16_t den, uint8_t exponent);
   bool read_res_box(uint32_t *id, uint32_t *num, uint32_t *den,
   		uint32_t *exponent, uint8_t **p_resolution_data);
   bool read_res( uint8_t *p_resolution_data,
   		uint32_t resolution_size);
   void find_cf(double x, uint32_t *num, uint32_t *den);
   void write_res_box(double resx, double resy, uint32_t box_id,
   		uint8_t **current_res_ptr);
   uint8_t* write_res( uint32_t *p_nb_bytes_written);
   uint8_t* write_bpc( uint32_t *p_nb_bytes_written);
   bool read_bpc( uint8_t *p_bpc_header_data,uint32_t bpc_header_size);
   uint8_t* write_channel_definition( uint32_t *p_nb_bytes_written);
   void apply_channel_definition(grk_image *image, grk_jp2_color *color);
   bool read_channel_definition( uint8_t *p_cdef_header_data,
   		uint32_t cdef_header_size);
   uint8_t* write_colr( uint32_t *p_nb_bytes_written);
   bool read_colr( uint8_t *p_colr_header_data,
   		uint32_t colr_header_size);
   bool check_color(grk_image *image, grk_jp2_color *color);
   bool apply_palette_clr(grk_image *image, grk_jp2_color *color);
   bool read_component_mapping( uint8_t *component_mapping_header_data,
   		uint32_t component_mapping_header_size);
   uint8_t* write_component_mapping( uint32_t *p_nb_bytes_written);
   uint8_t* write_palette_clr( uint32_t *p_nb_bytes_written);
   bool read_palette_clr( uint8_t *p_pclr_header_data,	uint32_t pclr_header_size);
   bool write_jp2h(void);
   bool write_uuids(void);
   bool write_ftyp(void);
   bool write_jp2c(void);
   bool write_jp(void);
   bool exec( std::vector<jp2_procedure> *procs) ;
   const grk_jp2_header_handler* find_handler(uint32_t id);
   const grk_jp2_header_handler* img_find_handler(uint32_t id);
   bool read_jp( uint8_t *p_header_data,uint32_t header_size);
   bool read_ftyp( uint8_t *p_header_data,	uint32_t header_size) ;
   bool skip_jp2c(void) ;
   bool read_jp2h( uint8_t *p_header_data,	uint32_t header_size);
   bool read_box(grk_jp2_box *box, uint8_t *p_data,
   		uint32_t *p_number_bytes_read, uint64_t p_box_max_size);
   void serializeAsoc(grk_jp2_asoc *asoc,
		   	   	   	   grk_asoc *serial_asocs,
					   uint32_t *num_asocs,
					   uint32_t level);

	/** handle to the J2K codec  */
	CodeStream *codeStream;
	/** list of validation procedures */
	std::vector<jp2_procedure> *m_validation_list;
	/** list of execution procedures */
	std::vector<jp2_procedure> *m_procedure_list;

	/* width of image */
	uint32_t w;
	/* height of image */
	uint32_t h;
	/* number of components in the image */
	uint16_t numcomps;
	uint8_t bpc;
	uint8_t C;
	uint8_t UnkC;
	uint8_t IPR;
	uint8_t meth;
	uint8_t approx;
	GRK_ENUM_COLOUR_SPACE enumcs;
	uint8_t precedence;
	uint32_t brand;
	uint32_t minversion;
	uint32_t numcl;
	uint32_t *cl;
	grk_jp2_comps *comps;
	uint64_t j2k_codestream_offset;
	bool needs_xl_jp2c_box_length;
	uint32_t jp2_state;
	uint32_t jp2_img_state;
	grk_jp2_color color;

	bool has_capture_resolution;
	double capture_resolution[2];

	bool has_display_resolution;
	double display_resolution[2];

	grk_jp2_buffer xml;
	grk_jp2_uuid uuids[JP2_MAX_NUM_UUIDS];
	uint32_t numUuids;

	grk_jp2_asoc root_asoc;
private:
	bool postDecompress( grk_image *p_image);
};


/** @name Exported functions */
/*@{*/
/* ----------------------------------------------------------------------- */

/*@}*/

/*@}*/

}

