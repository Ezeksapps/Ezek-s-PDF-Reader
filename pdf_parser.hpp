#ifndef PDF_PARSER_HPP
#define PDF_PARSER_HPP

#pragma once

/* VERSION 0.1_01, THIS LIBRARY IS IN 'ALPHA' & SHOULD NOT BE USED SERIOUSLY ASIDE FROM IN TESTING DUE TO BUGINESS & MISSING FEATURES 
this PDF parser as of now supports:
- text parsing, with fonts, text size & coordinates
- image XObject parsing assuming it is encoded in RGB with DEFLATE algorithm 
- can parse all standard PDF files version 1.5+, also supports xref streams & compression of objects, but does not support linearisation & non-standard or
more infrequents formats such as PDF/A, may also sometimes have trouble on certain adobe generated PDFs due to acrobat's tendency to use strange layouts or structs
- is for now, only a viewer, not an editor

The library itself has been tested on a few basic PDF documents, real-world testing was done where a PDF representing a Twinkl(R) worksheet was parsed
without any issue. This library is known to crash when:
- contents are in the non-standard format of having reference to more than one stream object
- xrefs are in a non-standard format
- any data is corrupted (even 1 byte of corrupted data could cause this thing to crash)
- encountering any kind of unexpected tag in an object or unexpected value

PLANS:
- support form functinality (AcroForms)
- support basic table / rect rendering
- improve stability

Linearisation is a more complex feature that may or may not be supported later on, but for now only small code fragments exist as a foundation if this is ever added
*/

/* This is a file of the PDF_Coder library */

#include <boost/regex.hpp> // STL regex is not very well optimised & is not overall that good of a parser. Boost's regex lib is better

/* c++ STL */
#include <string>
#include <vector>
#include <fstream>
#include <map>
#include <array>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstdint> // for uint8_t & uint64_t

/* zlib handles stream compression & decompression using the DEFLATE algorithm. It is a native linux lib */
#include <zlib.h>

namespace pdf_parser {

	enum streamFilter : int {
		FLATE_DECODE_FILTER
	};

	enum streamPredictor : int {
		PNG_NONE,
		PNG_SUB,
		PNG_UP,
		PNG_AVERAGE,
		PNG_PAETH,
		PNG_OPTIMUM
	};

	enum colour_space : int {
		DEVICE_RGB,
		DEVICE_CMYK
	};

	/* graphic state structs */

	struct transformationMatrix {
        double scale_x;     // Scale factor in the x-direction
        double shear_y;     // Shear factor in the y-direction
        double shear_x;     // Shear factor in the x-direction
        double scale_y;     // Scale factor in the y-direction
        double translate_x; // Translation in the x-direction
        double translate_y; // Translation in the 
	};

	struct colour {
        int r;
		int g;
		int b;
	};

	struct graphicsState {
        transformationMatrix ctm; // CTM (Current Transformation Matrix)
        colour stroke_colour;
		colour fill_colour;
		double line_width;
		double miter_limit;
	};

	struct object_hash {
		std::size_t operator()(int obj_num) const {
			return std::hash<int>{}(obj_num);
		}
	};

	// for image XObjects
	struct imageObject {
		graphicsState graphics_state;
		std::vector<uint8_t> image_stream; // uint8_t is better for raw byte data
		int width;
		int height;
		int bits_per_component;
		bool interpolate;
		colour_space clr_space;
		streamFilter filter;
	};

	/* graphics-related structs */

	struct coordinates {
		double x;
		double y;
	};

	struct rect {
		coordinates top_right;
		coordinates bottom_left;
	};

	/* text-related structures */

	struct fontObject {
		std::string font_name;
		int subtype;
	};

	struct textData {
		std::string text;
		int text_size;
		std::shared_ptr<fontObject> font;
	};

	struct textObject {
		coordinates text_coordinates;
		std::vector<textData> text_blocks;
	};

	/* External objects */

	struct xObject {
		std::string ref_id;
		std::size_t pos;
	};
	
	class page {
    public:
		page(std::size_t page_ref);
		~page();
		std::vector<imageObject> parse_page_images();
        std::vector<textObject> parse_text_objects(); // parse text objects inside a stream
		rect get_media_box();

	private:
	    struct pageContent {
		    std::string stream;
		    streamFilter filter;
	    };

		pageContent parse_content_stream(std::size_t content_stream_ref); 

		std::string inflate_stream_to_str(const std::string &deflated_stream);					 // for contents streams
		std::vector<uint8_t> inflate_stream_to_raw(const std::vector<uint8_t> &deflated_stream); // for image streams

		std::shared_ptr<fontObject> load_font(const std::string &font_key);
        void check_x_obj_type();

		rect media_box;
		std::map<std::string, std::size_t> font_refs;
		std::map<std::string, std::size_t> x_obj_refs; // XObjects
		std::size_t content_refs; // refs to contents obj
		int object_gen_number;
		std::string object_contents;
        
		pageContent contents; // content stream
		std::map<std::string, std::shared_ptr<fontObject>> font_cache;

        /* XObjects can either be forms or images, when a ref to an XObject is found in a stream,
		what that XObject is must first be determined to properly parse it. So when XObject are first mapped with their keys & positions, the types
		of the mapped objects are briefly checked & keys associated with images are saved to image_keys, keys associated with forms to form_keys */
		std::vector<std::string> image_keys; 
		std::vector<std::string> form_keys;
	};

	int open(std::string path);
	page get_page(int page_num);
	int get_num_pages();


}

#endif