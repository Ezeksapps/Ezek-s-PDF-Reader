#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <map>
#include <array>
#include <sstream>
#include <iostream>
#include <iomanip>
// zlib handles the DEFLATE compression algorithm used to compress streams
#include "zlib.h"

enum stream_filter : int {
	FLATE_DECODE_FILTER,
}; 

enum colour_space : int {
	DEVICE_RGB,
	DEVICE_CMYK,
};

struct object_hash {
	std::size_t operator()(int obj_num) const {
		return std::hash<int>{}(obj_num);
	}
};

struct xref_entry {
	std::size_t object_offset;
	int gen_num;
	char status;
};

struct object_ref {
	long offset;
	int generation_num;
	char status;
};

struct stream_object {
	std::string stream;
	stream_filter stream_filter;
	int object_gen_number;
};

struct image_object {
	std::vector<uint8_t> image_stream; // uint8_t is better for raw byte data
	int width;
	int height;
	int bits_per_component;
	bool interpolate;
	colour_space colour_space;
	stream_filter filter;
};

struct coordinates {
	double x;
	double y;
};

struct text_data {
	std::string text;
	int text_size;
	std::string used_font_id;
};

struct media_box {
	coordinates top_right;
	coordinates bottom_left;
};

struct text_object {
	coordinates text_coordinates;
	std::vector<text_data> text_data;
};

struct font_object {
	std::string font_id;
	std::string font_name;
	int subtype;
	int object_gen_number;
};

struct x_object {
	std::string ref_id;
	std::size_t pos;
};


struct page_object {
	media_box media_box;
	std::vector<font_object> fonts;
	std::vector<std::size_t> contents; // refs to contents
	std::vector<x_object> x_objects; // external object references
	int object_gen_number;
	std::string object_contents;
};

struct objects_root { // aka. the 'catalog' object, which is the root object representing the PDF
	int page_count;
	std::vector<std::size_t> pages; // refs to pages
	int object_gen_number;
	std::string object_contents;
};

struct doc_trailer {
	std::size_t startxref;
	std::pair<std::string, std::string> id; // the two-part document ID which is 33 chars long including \0
	int root_object_num;
	int info_object_num;
	int size;
};

class parser {
public:
	void open(std::string path);
	objects_root get_root();
	page_object parse_page(std::size_t page_ref);
	stream_object parse_content_stream(std::size_t content_stream_ref);
	std::vector<image_object> parse_page_images(page_object page);
	std::string get_pdf_version();

	// within streams:
	std::vector<text_object> parse_text_objects(std::string stream); // parse text objects inside a stream

private:
	std::string doc_contents; // the entire content of the document, without parsing or decompressing
	doc_trailer primary_trailer; // the principal document trailer
	std::map<int, xref_entry> object_refs; // the object positions

	std::string inflate_stream_to_str(const std::string& deflated_stream); // for contents streams
	std::vector<uint8_t> inflate_stream_to_raw(const std::vector<uint8_t>& deflated_stream); // for image streams

	std::vector<std::size_t> follow_refs(std::string refs_str, std::map<int, xref_entry> object_refs, std::size_t find_refs_from);
	std::string isolate_object_contents(std::string doc_contents, std::size_t object_offset);
	std::vector<std::pair<int, xref_entry>> parse_xref_table(std::size_t xref_pos, std::string doc_contents);
	int get_ref_object_num(std::string doc_contents, std::size_t find_refs_from);
	std::pair<std::string, std::string> get_doc_id(std::string doc_contents, std::size_t find_id_from);
	std::size_t get_xref_table_position();

	int get_tag_value(std::size_t tag_pos, std::string look_in);
	bool get_tag_bool_value(std::size_t tag_pos, std::string look_in);
	std::string get_tag_type(std::size_t tag_pos, std::string look_in);
};