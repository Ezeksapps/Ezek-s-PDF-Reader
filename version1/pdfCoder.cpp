#include "pdfCoder.hpp"

void parser::open(std::string path) {
    std::ifstream file(path, std::ios::binary);

    if (file.is_open()) {
        std::stringstream buffer;
        buffer << file.rdbuf();

        doc_contents = buffer.str();
        
        file.close();
    }

    // parse PDF's primary trailer
    std::size_t trailer_pos = doc_contents.rfind("trailer");
    if (trailer_pos != std::string::npos) {
        std::size_t root_object_ref = doc_contents.find("/Root", trailer_pos);
        primary_trailer.root_object_num = get_ref_object_num(doc_contents, root_object_ref);

        std::size_t info_object_ref = doc_contents.find("/Info", root_object_ref);
        primary_trailer.info_object_num = get_ref_object_num(doc_contents, info_object_ref);

        std::size_t id_pos = doc_contents.find("/ID", info_object_ref);
        primary_trailer.id = get_doc_id(doc_contents, id_pos);

        std::size_t startxref_pos = doc_contents.find("startxref", trailer_pos) + 10;
        primary_trailer.startxref = std::stoull(
            doc_contents.substr(startxref_pos, doc_contents.find_first_of(" ", startxref_pos) - startxref_pos)
        );
    }
}

std::string parser::get_pdf_version() {
    std::string version;
    std::size_t pos = doc_contents.find("%PDF-");
    if (pos != std::string::npos) {
        std::size_t endPos = doc_contents.find("\n", pos);
        version = doc_contents.substr(pos + 5, endPos - pos - 5);
    }
    return version;
}

// for old-style text xref tables, use parse_xref_stream() for the newer xref streams introduced in PDF 1.5
std::vector<std::pair<int, xref_entry>> parser::parse_xref_table(std::size_t xref_pos, std::string doc_contents) {
    std::vector<std::pair<int, xref_entry>> entries;

    std::istringstream iss(doc_contents, std::ios_base::in);
    iss.seekg(xref_pos); // go to xref position
    std::string line;

    // skip xref declaration & range lines
    std::getline(iss, line);
    std::getline(iss, line);

    while (true) {
        std::getline(iss, line);
        // check if the end of the xref table has been reached
        if (line == "trailer" || line == "xref") break;

        std::istringstream line_stream(line);
        std::string field_value;
        xref_entry entry;

        std::getline(line_stream, field_value, ' ');
        entry.object_offset = std::stoull(field_value);

        std::getline(line_stream, field_value, ' ');
        entry.gen_num = std::stoull(field_value);

        // Parse the type
        std::getline(line_stream, field_value, ' ');
        
        entries.emplace_back(entries.size(), entry);
    }
    // finally functional, make sure to make this more efficient later
    return entries;
}

objects_root parser::get_root() {
    std::size_t xref_table_position = get_xref_table_position();
    std::vector<std::pair<int, xref_entry>> entries = parse_xref_table(xref_table_position, doc_contents);
    for (std::pair<int, xref_entry> entry : entries) {
        object_refs.emplace(entry.first, entry.second);
    }

    objects_root objects_root;
    std::size_t root_object = 0;
    root_object = object_refs[primary_trailer.root_object_num].object_offset;
    objects_root.object_contents = isolate_object_contents(doc_contents, root_object);

    // find the pages object too
    std::size_t pages_object_ref = objects_root.object_contents.find("/Pages ", root_object);
    std::vector<std::size_t> pages_object_start = follow_refs(doc_contents, object_refs, pages_object_ref);
    // extract the positions of the individual page objects
    std::size_t pages_object_kids = doc_contents.find("/Kids", pages_object_start[0]);
    objects_root.pages = follow_refs(doc_contents, object_refs, pages_object_kids);
    // this is so the user knows how many times to invoke parse_page() for each page in the document
    objects_root.page_count = objects_root.pages.size();

    //PLAN, FIX ALL THE TREE-STRUCT PARSING THEN EXTRACT TEXT & OTHER BASICS. REFINE & CONVERT DOCXREADER TO NEW FORMAT & WORK ON APP STABILITY
    // NEW APP FEATURES: TDLI FIX & USE AS FRAMEWORK TO BUILD CLIENT ORGANISER & REQUEST LIST WHICH WILL ABOLISH THE TABLE, WHICH WAS NEVER A GOOD
    // IDEA IN THE FIRST PLACE. ONCE THE .PDF & .DOCX READERS ARE DONE, THE FILE READER FEATURE WILL BE COMPLETED
    return objects_root;
}

page_object parser::parse_page(std::size_t page_ref) {
    page_object page;
    page.object_contents = isolate_object_contents(doc_contents, page_ref);
    /*3 0 obj<</Type/Page/MediaBox[0 0 612 792]/Resources<</Font<</F0 4 0 R/F1 5 0 R>>>>/Parent 2 0 R / Contents 6 0 R >>
        endobj
    */
    // get the media box
    media_box media_box;
    std::size_t media_box_pos = page.object_contents.find("/MediaBox") + 10;
    std::string coords = page.object_contents.substr(media_box_pos, page.object_contents.find_first_of("]", media_box_pos) - media_box_pos);
    std::istringstream coords_stream (coords);
    coords_stream >> std::ws >> media_box.bottom_left.x
        >> std::ws >> media_box.bottom_left.y
        >> std::ws >> media_box.top_right.x
        >> std::ws >> media_box.top_right.y;
    page.media_box = media_box;
    
    std::size_t resources_pos = page.object_contents.find("/Resources"); // keep if needed in future

    // parse font objects for page
    std::size_t fonts_pos = page.object_contents.find("/Font", resources_pos);
    std::vector<std::size_t> font_positions = follow_refs(page.object_contents, object_refs, fonts_pos);
    std::size_t cur_font_id_pos = page.object_contents.find_first_of("/", fonts_pos + 1) + 1; // start cur_font_id_pos at first font's ID
    // go to each font object, isolate contents & parse
    for (std::size_t cur_font : font_positions) {
        font_object font;
        std::string object_contents = isolate_object_contents(doc_contents, cur_font);
        // get subtype number
        std::size_t subtype_num_pos = object_contents.find_first_of("1234567890", object_contents.find("/Subtype"));
        font.subtype = std::stoi(object_contents.substr(
            subtype_num_pos, object_contents.find_first_of("/> ", subtype_num_pos) - subtype_num_pos)
        );
        // get base font name
        std::size_t base_font_pos = object_contents.find("/BaseFont", 0) + 11;
        font.font_name = object_contents.substr(base_font_pos, object_contents.find_first_of("/> ", base_font_pos) - base_font_pos);
        // assgin font's identifier
        font.font_id = page.object_contents.substr(
            cur_font_id_pos, page.object_contents.find_first_of(" ", cur_font_id_pos) - cur_font_id_pos
        );
        page.fonts.push_back(font);
        cur_font_id_pos = page.object_contents.find_first_of("/", cur_font_id_pos) + 1;
    }

    // parse external objects (if any) LOGIC FAULTY
    std::size_t x_objects_pos = page.object_contents.find("/XObject");
    if (x_objects_pos != std::string::npos) {
        std::vector<std::size_t> x_object_positions = follow_refs(page.object_contents, object_refs, x_objects_pos);
        std::size_t cur_x_object_id_pos = page.object_contents.find_first_of("/", x_objects_pos + 1) + 1;
        for (int i = 0; i < x_object_positions.size(); ++i) {
            x_object x_object;
            x_object.pos = x_object_positions[i];
            x_object.ref_id = page.object_contents.substr(
                cur_x_object_id_pos, page.object_contents.find_first_of(" ", cur_x_object_id_pos) - cur_x_object_id_pos
            );
            cur_x_object_id_pos = page.object_contents.find_first_of("/", cur_x_object_id_pos) + 1;
            page.x_objects.push_back(x_object);
        }
    }

    std::size_t content_refs_pos = page.object_contents.find("/Contents");
    page.contents = follow_refs(page.object_contents, object_refs, content_refs_pos);
    
    return page;
}

stream_object parser::parse_content_stream(std::size_t content_stream_ref) {
    stream_object stream;
    std::string object_contents = isolate_object_contents(doc_contents, content_stream_ref);
    // decompress & save stream
    std::size_t stream_pos = object_contents.find("stream", 0) + 7;
    stream.stream = inflate_stream_to_str(object_contents.substr(stream_pos, object_contents.find("endobj", stream_pos) - 1 - stream_pos));
    //std::cout << stream.stream;
    return stream;
}

// Now that we finally have the decompressed stream with the new method, rewrite the parse_text_objects() function & add new features including:
// parsing of interactive boxes, parsing of normal rects, parsing of pictures, parsing bold & italic text, etc. 

std::size_t parser::get_xref_table_position() {
    return primary_trailer.startxref;
}

// old text parsing function:
/* 
std::vector<text_object> parser::parse_text_objects(std::string stream) {
    std::vector<text_object> text_objs;
    std::string text_obj_content;

    std::size_t obj_lookup_start = 0;

    std::size_t text_obj_begin;
    while ((text_obj_begin = stream.find("BT", obj_lookup_start)) != std::string::npos) {
        std::size_t text_obj_end = stream.find("ET", text_obj_begin);
        text_obj_content = stream.substr(text_obj_begin + 2, text_obj_end - text_obj_begin - 2);

        text_object obj;

        std::size_t find_coords_from = 0;
        while ((find_coords_from = text_obj_content.find("Td", find_coords_from)) != std::string::npos) {
            coordinates coords;

            std::size_t read_y_from = text_obj_content.rfind(" ", find_coords_from - 2);
            coords.y = std::stod(text_obj_content.substr(read_y_from + 1, find_coords_from - read_y_from - 2));

            std::size_t read_x_from = text_obj_content.rfind(" ", read_y_from - 1);
            coords.x = std::stod(text_obj_content.substr(read_x_from + 1, read_y_from - read_x_from - 1));

            obj.text_coordinates.push_back(coords);

            find_coords_from += 2; // Advance findCoordsFrom to search beyond the current "Td"
        }

        std::size_t find_text_from = 0;
        std::size_t text_symbol;
        std::string text;

        while ((text_symbol = text_obj_content.find("Tj", find_text_from)) != std::string::npos) {
            std::size_t read_text_from = text_obj_content.rfind("(", text_symbol);
            text += text_obj_content.substr(read_text_from + 1, text_symbol - read_text_from - 2);
            find_text_from = text_symbol + 2;
        }
        obj.text = text;

        obj_lookup_start = text_obj_end;
        text_objs.push_back(obj);
    }

    return text_objs;
}*/

std::vector<text_object> parser::parse_text_objects(std::string stream) {
    std::vector<text_object> text_objs;
    std::size_t object_lookup_start = 0;

    while (true) {
        std::size_t text_object_begin = stream.find("BT", object_lookup_start);
        if (text_object_begin == std::string::npos) break;
        std::size_t text_object_end = stream.find("ET", text_object_begin);

        std::string text_object_content = stream.substr(text_object_begin + 2, text_object_end - text_object_begin - 2);
        text_object text_object;

        std::istringstream parsing_stream(text_object_content);
        std::string field_value;
        std::size_t coords_symbol_pos = text_object_content.find("Td", 0);
        if (coords_symbol_pos != std::string::npos) {
            parsing_stream.seekg(text_object_content.rfind("\n", coords_symbol_pos) + 1);
            coordinates coords;
            std::getline(parsing_stream, field_value, ' ');
            coords.x = std::stod(field_value);
            std::getline(parsing_stream, field_value, ' ');
            coords.y = std::stod(field_value);
            text_object.text_coordinates = coords;
        }

        // find where all the text blocks are in the object
        std::vector<std::size_t> text_block_positions;
        int pos = 0;
        while (true) {
            std::size_t font_decl_pos = text_object_content.find("Tf", pos);
            if (font_decl_pos == std::string::npos) break;
            text_block_positions.push_back(text_object_content.rfind("\n", font_decl_pos) + 2);
            pos = font_decl_pos + 2;
        }

        // loop through text blocks
        int times_looped = 0;
        for (std::size_t cur_text_block : text_block_positions) {
            text_data text_data;
            // parse font & size for text block
            parsing_stream.seekg(cur_text_block);
            std::getline(parsing_stream, field_value, ' ');
            text_data.used_font_id = field_value;
            std::getline(parsing_stream, field_value, ' ');
            text_data.text_size = std::stoi(field_value);
            // get text
            std::size_t find_text_from = 0;
            std::size_t text_symbol = 0;
            std::string text;
            while ((text_symbol = text_object_content.find("Tj", find_text_from)) != std::string::npos) {
                parsing_stream.seekg(text_object_content.rfind("(", text_symbol) + 1);
                std::getline(parsing_stream, field_value, ')');
                text += field_value;
                find_text_from = text_symbol + 2;
            }
            text_data.text = text;
            text_object.text_data.push_back(text_data);
            ++times_looped;
        }
        text_objs.push_back(text_object);
        object_lookup_start = text_object_end;
    }
    return text_objs;
}

std::vector<image_object> parser::parse_page_images(page_object page) {
    std::vector<image_object> images;
    for (int i = 0; i < page.x_objects.size(); ++i) {
        if (doc_contents.find("/Image", page.x_objects[i].pos)) {
            image_object image;
            std::string object_contents = isolate_object_contents(doc_contents, page.x_objects[i].pos);
            // extract width & height
            std::size_t width_pos = object_contents.find("/Width", 0);
            image.width = get_tag_value(width_pos, object_contents);
            std::size_t height_pos = object_contents.find("/Height", 0);
            image.height = get_tag_value(height_pos, object_contents);
            // extract bits per component
            std::size_t bits_per_component_pos = object_contents.find("/BitsPerComponent", 0);
            image.bits_per_component = get_tag_value(bits_per_component_pos, object_contents);
            // get colour space
            std::size_t colour_space_pos = object_contents.find("/ColorSpace", 0);
            std::string colour_space = get_tag_type(colour_space_pos, object_contents);
            if (colour_space == "/DeviceRGB") image.colour_space = DEVICE_RGB;
            if (colour_space == "/DeviceCMYK") image.colour_space = DEVICE_CMYK;
            // check interpolate bool value
            std::size_t interpolate_pos = object_contents.find("/Interpolate", 0);
            image.interpolate = get_tag_bool_value(interpolate_pos, object_contents);
            // decompress image stream
            std::size_t stream_pos = object_contents.find("stream", 0) + 7;
            std::size_t endstream_pos = object_contents.find("endstream", stream_pos) - 1;
            if (endstream_pos != std::string::npos) {
                image.image_stream = inflate_stream_to_raw(std::vector<uint8_t>(object_contents.begin() + stream_pos, object_contents.begin() + endstream_pos));
            }
            images.push_back(image);
        }
    }
    return images;
}

std::pair<std::string, std::string> parser::get_doc_id(std::string doc_contents, std::size_t find_id_from) {
    // ID part 1
    std::size_t id_start = doc_contents.find("<", find_id_from) + 1;
    std::size_t id_end = doc_contents.find(">", id_start);
    std::string id_1 = doc_contents.substr(id_start, id_end - id_start);

    // ID part 2
    id_start = doc_contents.find("<", id_end) + 1;
    id_end = doc_contents.find(">", id_start);
    std::string id_2 = doc_contents.substr(id_start, id_end - id_start);

    return std::make_pair(id_1, id_2);
}

// when instead of following the ref, we only want to extract the number of the refrenced object
int parser::get_ref_object_num(std::string doc_contents, std::size_t find_refs_from) {
    std::size_t start_ref = doc_contents.find_first_of("1234567890", find_refs_from);
    std::size_t end_ref = doc_contents.find_first_not_of("1234567890 ", start_ref);
    return std::stoi(doc_contents.substr(start_ref, doc_contents.find_first_of(" ", start_ref) - start_ref));
}

std::vector<std::size_t> parser::follow_refs(std::string ref_str, std::map<int, xref_entry> object_refs, std::size_t find_refs_from) {
    std::vector<std::size_t> refs;
    std::size_t pos = ref_str.find_first_of("123456789", find_refs_from); // there is never a 0 object, so don't look for 0
    if (std::isspace(ref_str[pos - 1]) || std::ispunct(ref_str[pos - 1])) {
        while (std::isdigit(ref_str[pos])) {
            // Formats: /<tag_name> <ref> <ref> (etc.) and /<tag_name>[<ref>, <ref>, (etc.)]
            std::size_t end_ref = ref_str.find_first_not_of("1234567890 ", pos);
            std::size_t ref = object_refs[std::stoi(ref_str.substr(pos, ref_str.find_first_of(" ", pos) - pos))].object_offset;
            refs.push_back(ref);
            pos = ref_str.find_first_of("1234567890]>/", end_ref);
        }
    }
    if (std::isalpha(ref_str[pos - 1])) {
        while (isdigit(ref_str[pos])) {
            pos = ref_str.find_first_of(" ", pos) + 1; // readjust the position to the actual ref start instead of the font ID number
            std::size_t end_ref = ref_str.find_first_not_of("1234567890 ", pos);
            std::size_t ref = object_refs[std::stoi(ref_str.substr(pos, ref_str.find_first_of(" ", pos) - pos))].object_offset;
            refs.push_back(ref);
            // this will never land on the actual ref start which is why the readjustment above exists, if '>' is found, then no more refs exist
            pos = doc_contents.find_first_of("1234567890>", end_ref);
        }
    }
    return refs;
}

std::string parser::isolate_object_contents(std::string doc_contents, std::size_t object_offset) {
    return doc_contents.substr(object_offset, doc_contents.find("endobj", object_offset) - object_offset);
}

std::vector<uint8_t> parser::inflate_stream_to_raw(const std::vector<uint8_t>& deflated_stream) {
    z_stream stream{};
    int ret = inflateInit(&stream);
    if (ret != Z_OK) std::cout << "\ncould not init zlib\n";

    std::vector<uint8_t> inflated_stream;
    const int chunk_size = 16384; // 16 KB chunk size
    std::vector<uint8_t> buffer(chunk_size);

    stream.next_in = const_cast<Bytef*>(deflated_stream.data());
    stream.avail_in = static_cast<uInt>(deflated_stream.size());

    while (ret != Z_STREAM_END) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = chunk_size;

        ret = inflate(&stream, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            std::cerr << "Error: zlib stream error. Error code: " << ret << std::endl;
            inflateEnd(&stream);
            return {}; // Return an empty vector on error
        }

        inflated_stream.insert(inflated_stream.end(), buffer.data(), buffer.data() + chunk_size - stream.avail_out);
    }

    inflateEnd(&stream);

    return inflated_stream;
}

std::string parser::inflate_stream_to_str(const std::string& deflated_stream) {
    z_stream zs{};
    zs.zalloc = Z_NULL;
    zs.zfree = Z_NULL;
    zs.opaque = Z_NULL;
    zs.avail_in = 0;
    zs.next_in = Z_NULL;

    int ret = inflateInit(&zs); // Allow zlib and gzip decoding
    if (ret != Z_OK) {
        throw std::runtime_error("inflateInit2 failed");
    }
    std::string decompressed_data;
    std::vector<char> out_buffer(32768);

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(deflated_stream.data()));
    zs.avail_in = static_cast<uInt>(deflated_stream.size());

    do {
        zs.next_out = reinterpret_cast<Bytef*>(out_buffer.data());
        zs.avail_out = static_cast<uInt>(out_buffer.size());

        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR) {
            inflateEnd(&zs);
            throw std::runtime_error("inflate failed");
        }

        decompressed_data.insert(decompressed_data.end(), out_buffer.begin(), out_buffer.begin() + out_buffer.size() - zs.avail_out);
    } while (ret != Z_STREAM_END);
    std::cout << "ending...";

    inflateEnd(&zs);
    return decompressed_data;
}

int parser::get_tag_value(std::size_t tag_pos, std::string look_in) {
    while (!std::isdigit(look_in[tag_pos])) ++tag_pos;
    std::size_t tag_end = look_in.find_first_of("/", tag_pos);
    return std::stoi(look_in.substr(tag_pos, tag_end - tag_pos));
}

bool parser::get_tag_bool_value(std::size_t tag_pos, std::string look_in) {
    if (look_in.find("true", tag_pos) != std::string::npos) return true;
    else return false;
}

std::string parser::get_tag_type(std::size_t tag_pos, std::string look_in) {
    std::size_t type_pos = look_in.find_first_of("/", tag_pos + 1);
    std::size_t type_end = look_in.find_first_of("/", type_pos + 1);
    return look_in.substr(type_pos, type_end - type_pos);
}