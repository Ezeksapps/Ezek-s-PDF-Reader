#include "pdf_parser.hpp"

namespace pdf_parser {

    /* struct definitions not exposed to API */

    // used to store deflated (or compressed) objects' refs in an xrefStream for parsing in parse_xref_stream()
    struct deflatedObjRef {
        int index;
        std::size_t line_start;
        std::size_t line_end;
    };

    struct xrefEntry {
		std::size_t object_offset;
		int gen_num;
		char status;
	};

    /* used in other structs to store obj & gen num of specific refrenced objects */
	struct objectRef {
		int obj_num;
		int gen_num;
	};
    
    // PDFs can either store essential ref data in trailers (since PDF 1.0) or xrefStreams (since 1.5) this base allows both options to derive from a common parent
    struct refStruct {
        virtual ~refStruct() = default;
        // variables common to both derived structs
        std::size_t startxref;
        std::array<std::string, 2> id; // the two-part document ID which is 33 chars long including \0
        objectRef root_object_ref;
		objectRef info_object_ref;
    };

    struct xrefStreamInfo {
        streamPredictor predictor;
        int columns; // used only when a predictor is used
        streamFilter filter;
        std::array<int, 2> index; // key-value pair in xrefStreams that represents starting object & amount of objects stored
        std::array<int, 3> width;
    };

    // holds document trailer
	struct docTrailer : public refStruct {
        objectRef encrypt; // not currenly implements. contains ref to encryption data
	};
    

	// holds root object
	struct objectsRoot { // aka. the 'catalog' object, which is the root object representing the PDF
		int page_count;
		std::vector<std::size_t> pages; // refs to pages
		int object_gen_number;
		std::string object_contents;
	};

    struct docCore {
        std::string doc_contents;  // The entire content of the document
        refStruct ref_struct; // the document's primary ref struct, can either be a traler or xrefStream
        std::map<int, std::map<int, xrefEntry>> object_refs; // xref object references to lookup objects
        objectsRoot objects_root;
    };

    docCore doc_core {}; // contains essential document components, initialised to defaults, updated whenever a PDF is opened


    std::string isolate_object_contents(const std::string& main_str, std::size_t object_offset) {
        return main_str.substr(object_offset, main_str.find("endobj", object_offset) - object_offset);
    }

    /* from PDF 1.5 onwards, PDFs can compress most of their objects into a stream contained in an object give the type: /ObjStm
    this function will decompress these /ObjStm objects & update doc_contents to store the PDF in its standard form. used in parse_xref_stream() */
    std::string inflate_obj_stream(int obj_num) { 
        boost::regex obj_stream_regex(std::to_string(obj_num) + R"(\s+\d+\s+obj\s*<<[^>]*?/Type\s*/ObjStm[^>]*?>>\s*stream\r?\n([\s\S]*?)\r?\nendstream\r?\nendobj)");
        boost::smatch obj_match;
        boost::regex_search(doc_core.doc_contents, obj_match, obj_stream_regex);
        std::string obj_stream = obj_match[1];

        std::cout << "there is one of these";

        z_stream zs{};
        zs.zalloc = Z_NULL;
        zs.zfree = Z_NULL;
        zs.opaque = Z_NULL;
        zs.avail_in = 0;
        zs.next_in = Z_NULL;

        int ret = inflateInit2(&zs, 15 + 32); // Allow zlib and gzip decoding
        if (ret != Z_OK) {
            throw std::runtime_error("inflateInit2 failed");
        }

        std::string decompressed_stream;
        std::vector<char> out_buffer(32768);

        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(obj_stream.data()));
        zs.avail_in = static_cast<uInt>(obj_stream.size());

        do {
            zs.next_out = reinterpret_cast<Bytef*>(out_buffer.data());
            zs.avail_out = static_cast<uInt>(out_buffer.size());

            ret = inflate(&zs, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                inflateEnd(&zs);
                throw std::runtime_error("inflate failed");
            }

            decompressed_stream.insert(decompressed_stream.end(), out_buffer.begin(), out_buffer.begin() + out_buffer.size() - zs.avail_out);
        } while (ret != Z_STREAM_END);

        inflateEnd(&zs);

        return decompressed_stream;
    }

    int get_tag_value(std::size_t tag_pos, const std::string& look_in) {
        while (!std::isdigit(look_in[tag_pos])) ++tag_pos;
        std::size_t tag_end = look_in.find_first_of("/", tag_pos);
        return std::stoi(look_in.substr(tag_pos, tag_end - tag_pos));
    }

    bool get_tag_bool_value(std::size_t tag_pos, const std::string& look_in) {
        if (look_in.find("true", tag_pos) != std::string::npos) return true;
        else return false;
    }



    std::string get_tag_type(std::size_t tag_pos, const std::string& look_in) {
        std::size_t type_pos = look_in.find_first_of("/", tag_pos + 1);
        std::size_t type_end = look_in.find_first_of("/", type_pos + 1);
        return look_in.substr(type_pos, type_end - type_pos);
    }

    std::size_t parse_obj_ref(const std::string& ref_tag, const std::string& look_in) {
        boost::regex ref_regex(ref_tag + R"(\s+(\d+)\s+(\d+)\s+R)");
        boost::smatch ref_match;
        boost::regex_search(look_in, ref_match, ref_regex);
        return doc_core.object_refs[std::stoi(ref_match[1])][std::stoi(ref_match[2])].object_offset;
    }

    std::vector<std::size_t> parse_obj_ref_array(const std::string& ref_tag, const std::string& look_in) {
        std::vector<std::size_t> objs;

        // Regex to find the array
        boost::regex array_regex(ref_tag + R"(\s*\[\s*((?:\d+\s+\d+\s+R\s*)+)\])");
        boost::smatch array_match;

        if (boost::regex_search(look_in, array_match, array_regex)) {
            std::string array_content = array_match[1].str();
            boost::regex ref_regex(R"((\d+)\s+(\d+)\s+R)");
            boost::sregex_iterator iter(array_content.begin(), array_content.end(), ref_regex);
            boost::sregex_iterator end;
            while (iter != end) {
                objs.push_back(doc_core.object_refs[std::stoi((*iter)[1])][std::stoi((*iter)[2])].object_offset);
                ++iter;
            }
        }

        return objs;
    }

    // rewrite with more effcient approach later
    std::map<std::string, std::size_t> parse_obj_ref_dict(const std::string& dict_tag, const std::string& look_in) {
        std::map<std::string, std::size_t> obj_map;

        // Construct the regex pattern
        boost::regex dict_regex(dict_tag + R"(\s*<<\s*((?:/\w+\s+\d+\s+\d+\s*R\s*)+)>>)");

        boost::sregex_iterator iter(look_in.begin(), look_in.end(), dict_regex);
        boost::sregex_iterator end;

        for (; iter != end; ++iter) {
            std::string dict_content = (*iter)[1];

            // Regex to find individual references within the dictionary content
            boost::regex ref_regex(R"(/(\w+)\s+(\d+)\s+(\d+)\s*R\s*)");
            boost::sregex_iterator ref_iter(dict_content.begin(), dict_content.end(), ref_regex);
            boost::sregex_iterator ref_end;

            for (; ref_iter != ref_end; ++ref_iter) {
                obj_map.emplace((*ref_iter)[1], doc_core.object_refs[std::stoi((*ref_iter)[2])][std::stoi((*ref_iter)[3])].object_offset);
            }
            return obj_map;
        }
    }

    std::size_t get_xref_table_position() {
        return doc_core.ref_struct.startxref;
    }

    rect parse_rect(const std::string& rect_tag, const std::string& look_in) {
        rect parsed_rect;

        // Updated regex to handle both integers and decimal numbers
        boost::regex rect_regex(rect_tag + R"(\s*\[\s*([-+]?[0-9]*\.?[0-9]+)\s*([-+]?[0-9]*\.?[0-9]+)\s*([-+]?[0-9]*\.?[0-9]+)\s*([-+]?[0-9]*\.?[0-9]+)\s*\])");
        boost::smatch match;

        if (boost::regex_search(look_in, match, rect_regex)) {
            parsed_rect.bottom_left.x = std::stod(match[1]);
            parsed_rect.bottom_left.y = std::stod(match[2]);
            parsed_rect.top_right.x = std::stod(match[3]);
            parsed_rect.top_right.y = std::stod(match[4]);
        }
        return parsed_rect;
    }

    
    // for old-style text xref tables, use parse_xref_stream() for the newer xref streams introduced in PDF 1.5
    void parse_xref_table(std::size_t xref_pos) {
        std::istringstream iss(doc_core.doc_contents);
        iss.seekg(xref_pos); // go to xref position
        std::string line;

        std::getline(iss, line);  // skip xref symbol

        int cur_obj_num = 0;
      
        while (std::getline(iss, line)) {
            if (line == "trailer"  || line == "xref") break;

            // Parse subsection header, this tells us the number of the first object in the xref & the amount of objects in the xref
            int first_obj_num, obj_count;
            std::istringstream header_stream(line);
            header_stream >> first_obj_num >> obj_count;
            cur_obj_num = first_obj_num;


            for (int i = 0; i < obj_count; ++i) {
                std::getline(iss, line);
                std::istringstream line_stream(line);
                xrefEntry entry;

                std::cout << line << "\n";

                line_stream >> entry.object_offset >> entry.gen_num >> entry.status;

                if (entry.gen_num == 0) cur_obj_num = first_obj_num + i; // if the entry has a gen number of 0 we have a new obj ref, so ++ cur_obj_num

                doc_core.object_refs[cur_obj_num][entry.gen_num] = entry; // add entry to appropriate index
            }
            
        }
        for (const auto& ref : doc_core.object_refs) {
            std::cout << ref.first << " ";
            for (const auto& nested_ref : ref.second) {
                std::cout << nested_ref.first << " " << nested_ref.second.object_offset << "\n";
            }
        }
    }

    void parse_doc_trailer(const std::string& trailer_content) {
        boost::regex root_regex(R"(/Root\s+(\d+)\s+(\d+)\s+R)");
        boost::smatch root_match;
        if (boost::regex_search(trailer_content, root_match, root_regex)) {
            doc_core.ref_struct.root_object_ref.obj_num = std::stoi(root_match[1]);
            doc_core.ref_struct.root_object_ref.gen_num = std::stoi(root_match[2]);
        }

        // Parse Info object number
        boost::regex info_regex(R"(/Info\s+(\d+)\s+(\d+)\s+R)");
        boost::smatch info_match;
        if (boost::regex_search(trailer_content, info_match, info_regex)) {
            doc_core.ref_struct.info_object_ref.obj_num = std::stoi(info_match[1]);
            doc_core.ref_struct.info_object_ref.gen_num = std::stoi(info_match[2]);
        }

        // Parse ID
        boost::regex id_regex(R"(/ID\s*\[\s*<([^>]+)>\s*<([^>]+)>\s*\])");
        boost::smatch id_match;
        if (boost::regex_search(trailer_content, id_match, id_regex)) {
            doc_core.ref_struct.id = {id_match[1], id_match[2]};
        }
    }

    std::map<int, std::vector<deflatedObjRef>> get_deflated_obj_refs(const std::string& xref_stream) { // 
        std::map<int, std::vector<deflatedObjRef>> obj_refs; // Keyed by object stream number


        std::istringstream xref_str_stream(xref_stream);
        std::string line;
        std::streampos current_pos = 0;



        while (std::getline(xref_str_stream, line)) {
            std::streampos start_pos = current_pos;
            current_pos += line.size() + 1; // +1 for the newline character

            if (!line.empty() && line[0] == '2') { // only process lines beginning with '2'
                std::istringstream line_stream(line);
                std::string obj_num_str, index_str;

                // Ignore the first part (type) and read the next two parts
                line_stream.ignore(std::numeric_limits<std::streamsize>::max(), ' ');
                line_stream >> obj_num_str >> index_str;

                int obj_num = std::stoi(obj_num_str);
                int index = std::stoi(index_str);

                deflatedObjRef ref = {
                    index,
                    static_cast<std::size_t>(start_pos),
                    static_cast<std::size_t>(current_pos)
                };

                obj_refs[obj_num].push_back(ref);
            }
        }
    return obj_refs;
    }
    
    /* reinserts decompressed objects, the return is used in parse_xref_stream, where the array of size_t is the start & end of the entry associated with
    an object & the string, what should replace the old entry, as the entry no longer represents a type-2 compressed object */
    std::map<std::array<std::size_t, 2>, std::string> reinsert_inflated_objs(const std::string& obj_stream,
            const std::vector<deflatedObjRef>& deflated_obj_refs) {

        std::map<std::array<std::size_t, 2>, std::string> obj_entries;
        
        /* /ObjStm have a sequence of numbers at their beggining, these are key-value pairs where the key is the object number & the value,
        its offset in the stream. map each object to its offset */
        std::map<int, std::size_t> obj_map;
        std::istringstream stream(obj_stream);
        int key, offset;
        std::streampos objs_start = 0; // holds the current position of the stream, used to determine the end of the number header
        while (stream >> key >> offset) {
            obj_map[key] = offset;
            objs_start = stream.tellg(); // update objs_start to new offset
        }

        /* The offsets mapped with each object are relative to the beggining of actual object definition in the stream, not the stream's true beggining where
        the key-value pairs are held, therefore remove the key-value pairs from the stream */
        std::size_t pos = static_cast<std::size_t>(objs_start); // since streampos can't be used for string operations
        while (std::isspace(obj_stream[pos])) ++pos; // remove trailing whitespace for exact operations
        std::string objs = obj_stream.substr(static_cast<std::size_t>(pos));

        std::size_t xref_start; // holds where the xrefStream used to begin


        /* the xref stream object is no longer needed, remove it to clear space for reinsertion of the decompressed objs */
        boost::regex xref_stm_regex(R"(\d+\s+\d+\s+obj\s*<<((?:(?!/Type).)*?/Type\s*/XRef.*?>>\s*stream[\s\S]*?endstream)\s*endobj)");
        boost::smatch xref_stm_match;
        if (boost::regex_search(doc_core.doc_contents, xref_stm_match, xref_stm_regex)) {
            xref_start = xref_stm_match.position();
            std::size_t xref_length = xref_stm_match.length();
            doc_core.doc_contents.erase(xref_start, xref_length);
        }

        /* Now, go through each entry. each object will have its contents isolated from the stream & be placed back into the main document contents
        in its corresponding position */
        std::vector<std::size_t> obj_offsets;
        std::size_t insertion_offset = xref_start; // this is the position where the objects will be reinserted
        for (auto iter = obj_map.begin(); iter != obj_map.end(); ++iter) {
            auto next_iter = std::next(iter);
            std::size_t next_obj_offset = (next_iter != obj_map.end()) ? next_iter->second : objs.size();
            std::string obj_contents = objs.substr(iter->second, next_obj_offset - iter->second);
            std::string obj_sig(std::to_string(iter->first) + " 0 obj"); // since compressed objs are missing the '<obj num> <gen num> obj' prefix
            std::string obj_string = obj_sig + obj_contents + "\nendobj\n";
            doc_core.doc_contents.insert(insertion_offset, obj_string);
            obj_offsets.push_back(insertion_offset);
            insertion_offset += obj_string.length();
        }

        int cur_obj = 0;
        for (const auto& obj_ref : deflated_obj_refs) {
            std::ostringstream entry;
            entry << std::setw(10) << std::setfill('0') << obj_offsets[cur_obj] << " 00000 n \n";
            obj_entries.emplace(std::array{ obj_ref.line_start, obj_ref.line_end }, entry.str());
            ++cur_obj;
        }

        return obj_entries;
    }


    std::vector<uint8_t> apply_png_up_predictor(const std::vector<uint8_t>& input, int columns) {
        std::vector<uint8_t> output;
        output.reserve(input.size());

        // First row: assume Prior(x) = 0
        for (int i = 0; i < columns; ++i) {
            output.push_back(input[i]);
        }

        // Process subsequent rows
        for (std::size_t i = columns; i < input.size(); ++i) {
            std::size_t prior_index = i - columns;
            uint8_t predicted_byte = (input[i] + output[prior_index]) & 0xFF; // mod 256
            output.push_back(predicted_byte);
        }

        return output;
    }


    // used in parse_xref_stream, decompresses & structures the xref
    std::string inflate_xref_stream(const std::vector<uint8_t> stream, xrefStreamInfo stream_info) {
        z_stream zs{};
        int ret = inflateInit(&zs);

        std::vector<uint8_t> inflated_stream;
        const int chunk_size = 16384; // 16 KB chunk size
        std::vector<uint8_t> buffer(chunk_size);

        zs.next_in = const_cast<Bytef*>(stream.data());
        zs.avail_in = static_cast<uInt>(stream.size());

        while (ret != Z_STREAM_END) {
            zs.next_out = reinterpret_cast<Bytef*>(buffer.data());
            zs.avail_out = chunk_size;

            ret = inflate(&zs, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                inflateEnd(&zs);
                return {}; // Return an empty vector on error
            }

            inflated_stream.insert(inflated_stream.end(), buffer.data(), buffer.data() + chunk_size - zs.avail_out);
        }

       inflateEnd(&zs);

        
        switch (stream_info.predictor) {
        case PNG_NONE:
            break;
        case PNG_UP:
            inflated_stream = apply_png_up_predictor(inflated_stream, stream_info.columns);
            break;
        default:
            // no predictor
            break;
        }


        /* the decompressed stream is actually non-readable binary data that requires futher interpretation before becoming a readable xref */

        int status_width = stream_info.width[0];
        int offset_width = stream_info.width[1];
        int gen_num_width = stream_info.width[2];

        /* type refers to the obj's current type, 0 means it represents a free obj, 1 an in-use obj & 2 an obj still compressed in a /ObjStm
        for entries where type = 0:
         field_1 = obj num of next free obj
         field_2 = gen_num
        for entries where type = 1:
         field_1 = obj's byte offset
         field_2 = gen_num
        for entries where type = 2:
         field_1 = the obj number of the /ObjStm where the object is stored, gen_num is always assumed to be 0
         field_2 = object's index within the /ObjStm
        */

        struct entryValues {
            uint64_t type;
            uint64_t field_1;
            uint64_t field_2;
        };

        std::vector<entryValues> entries;
        std::size_t entry_size = status_width + offset_width + gen_num_width;

        for (std::size_t i = 0; i < inflated_stream.size(); i += entry_size) {
            entryValues entry;
            std::size_t pos = i;

            entry.type = 0;
            for (int j = 0; j < status_width; ++j) {
                entry.type = (entry.type << 8) | static_cast<unsigned char>(inflated_stream[pos++]);
            }

            entry.field_1 = 0;
            for (int j = 0; j < offset_width; ++j) {
                entry.field_1 = (entry.field_1 << 8) | static_cast<unsigned char>(inflated_stream[pos++]);
            }

            entry.field_2 = 0;
            for (int j = 0; j < gen_num_width; ++j) {
                entry.field_2 = (entry.field_2 << 8) | static_cast<unsigned char>(inflated_stream[pos++]);
            }

            entries.push_back(entry);
        }

        std::ostringstream xref; // the reconstructed xref table

        for (const entryValues& entry : entries) {
            switch (entry.type) {
            case 0: // Free object
                xref << std::setw(10) << std::setfill('0') << entry.field_1 << " "
                    << std::setw(5) << std::setfill('0') << entry.field_2 << " f \n";
                break;
            case 1: // Used object
                xref << std::setw(10) << std::setfill('0') << entry.field_1 << " "
                    << std::setw(5) << std::setfill('0') << entry.field_2 << " n \n";
                break;
            case 2: // compressed object for extraction
                xref << entry.type << " " << std::setw(5) << std::setfill('0') << entry.field_1 << " "
                    << std::setw(10) << std::setfill('0') << entry.field_2 << " \n";
            default:
                break;
            }
        }

        return xref.str();
    }
    

    void reinsert_xref(const std::map<std::array<std::size_t, 2>, std::string>& inflated_entries, xrefStreamInfo stream_info, const std::string& xref_stream) {
        std::string xref = xref_stream;

        /* The /XRef object is no longer needed & will not be counted in the objects, its associated entry should therefore now be removed: */

        int startxref = doc_core.ref_struct.startxref; // the logged startxref refers to the offset of the /XRef obj, which should be deleted from the final xref
        // returns a stringstream containing the entry generated from the startxref value
        auto get_xref_obj_entry = [](int value) {
            return (std::ostringstream() << std::setw(10) << std::setfill('0') << value << " 00000 n").str();
        }; 
        // find & erase entry
        std::string obj_entry = get_xref_obj_entry(startxref);
        std::size_t pos = xref.find(obj_entry);
        if (pos != std::string::npos) {
            xref.erase(pos, obj_entry.size());
        }

        /* Update xref with decompressed objs' entries */

        for (auto iter = inflated_entries.begin(); iter != inflated_entries.end(); ++iter) {
            xref.erase(iter->first[0], iter->first[1] - iter->first[0]);
            xref.insert(iter->first[0], iter->second);
        }

        /* All xrefs start with the 'xref' symbol & the subsection header containing the no. of the first object represented & teh amount represented, so update
        xref to include this: */
        std::string xref_header("\nxref\n" + std::to_string(stream_info.index[0]) + " " + std::to_string(stream_info.index[1]) + "\n");
        xref.insert(0, xref_header);

        /* Now, reconstruct the trailer: */

        std::string root_obj_ref(" " + std::to_string(doc_core.ref_struct.root_object_ref.obj_num) + " " +
                                 std::to_string(doc_core.ref_struct.root_object_ref.gen_num) + " R");
        std::string info_obj_ref(" " + std::to_string(doc_core.ref_struct.info_object_ref.obj_num) + " " +
                                 std::to_string(doc_core.ref_struct.info_object_ref.gen_num) + " R");
        std::string id("[<" + doc_core.ref_struct.id[0] + "><" + doc_core.ref_struct.id[1] + ">]");
        std::string trailer("trailer\n<</Root" + root_obj_ref + "/Info" + info_obj_ref + "/ID" + id + ">>");

        /* proceed with reinsertion of xref & trailer: */

        std::size_t eof_pos = doc_core.doc_contents.rfind(R"(%%EOF)") - 1;
        std::size_t xref_pos = doc_core.doc_contents.rfind("endobj", eof_pos) + 6; 

        doc_core.doc_contents.erase(xref_pos, eof_pos - xref_pos);
        doc_core.doc_contents.insert(xref_pos, xref);
        std::size_t trailer_pos = xref_pos + xref.size();
        doc_core.doc_contents.insert(trailer_pos, trailer);
        doc_core.doc_contents.insert(trailer_pos + trailer.size(), std::string("\nstartxref\n" + std::to_string(xref_pos) + "\n"));
        // update startxref (+1 because the xref string contains a \n before the actual xref symbol, which is what the value of startxref should be)
        doc_core.ref_struct.startxref = xref_pos + 1;
    }
    
    // xref streams are a compressed & compacted form of the old-style xref tables introduced in 1.5, they also allow for object compression
    void parse_xref_stream(const std::string& obj_content) {
        boost::regex root_regex(R"(/Root\s+(\d+)\s+(\d+)\s+R)");
        boost::smatch root_match;
        if (boost::regex_search(obj_content, root_match, root_regex)) {
            doc_core.ref_struct.root_object_ref.obj_num = std::stoi(root_match[1]);
            doc_core.ref_struct.root_object_ref.gen_num = std::stoi(root_match[2]);
        }

        // Parse Info object number
        boost::regex info_regex(R"(/Info\s+(\d+)\s+(\d+)\s+R)");
        boost::smatch info_match;
        if (boost::regex_search(obj_content, info_match, info_regex)) {
            doc_core.ref_struct.info_object_ref.obj_num = std::stoi(info_match[1]);
            doc_core.ref_struct.info_object_ref.gen_num = std::stoi(info_match[2]);
        }

        boost::regex id_regex(R"(/ID\s*\[\s*<([^>]+)>\s*<([^>]+)>\s*\])");
        boost::smatch id_match;
        if (boost::regex_search(obj_content, id_match, id_regex)) {
            doc_core.ref_struct.id = {id_match[1], id_match[2]};
        }

        xrefStreamInfo stream_info;

        boost::regex decode_params_regex(R"(/DecodeParams<</Columns\s+(\d+)/Predictor\s+(\d+)>>)");
        boost::smatch decode_params_match;
        if (boost::regex_search(obj_content, decode_params_match, decode_params_regex)) {
            int columns = std::stoi(decode_params_match[1].str());
            stream_info.columns = columns;
            int predictor = std::stoi(decode_params_match[2].str());
            switch (predictor) {
            case 10:
                break;
            case 11:
                break;
            case 12:
                stream_info.predictor = PNG_UP;
                break;
            case 13:
                break;
            case 14:
                break;
            case 15:
                break;
            default:
                break;
            }
        }

        boost::regex width_regex(R"(/W\s*\[\s*(\d+)\s*(\d+)\s*(\d+)\s*\])");
        boost::smatch width_match;
        if (boost::regex_search(obj_content, width_match, width_regex)) {
            stream_info.width = { std::stoi(width_match[1]), std::stoi(width_match[2]), std::stoi(width_match[3]) };
        }
        boost::regex index_regex(R"(/Index\s*\[\s*(\d+)\s*(\d+)\s*\])");
        boost::smatch index_match;
        if (boost::regex_search(obj_content, index_match, index_regex)) {
            stream_info.index = { std::stoi(index_match[1]), std::stoi(index_match[2]) };
        }
        
        boost::regex stream_regex(R"(stream\s*\n((?:(?!endstream)[\s\S])*)\s*endstream)");
        boost::smatch stream_match;
        if (boost::regex_search(obj_content, stream_match, stream_regex)) {

            std::string xref_stream = inflate_xref_stream(std::vector<uint8_t>(stream_match[1].begin(), stream_match[1].end()), stream_info);
            std::cout << xref_stream;
            std::map<int, std::vector<deflatedObjRef>> deflated_obj_refs = get_deflated_obj_refs(xref_stream);
            for (const auto& ref : deflated_obj_refs) {
                std::string obj_stream = inflate_obj_stream(ref.first);
                std::map<std::array<std::size_t, 2>, std::string> inflated_obj_entries = reinsert_inflated_objs(obj_stream, ref.second);        
                reinsert_xref(inflated_obj_entries, stream_info, xref_stream);
            }
        }
    }

    // TO BE COMPLETED
    void prepare_linearised_pdf(const std::string& linearisation_header) {
        boost::regex hint_tbl_ref_regex(R"(/H\s*\[\s*(\d+)\s*(\d+)\s*\])");
        boost::smatch hint_tbl_ref_match;
        boost::regex_search(linearisation_header, hint_tbl_ref_match, hint_tbl_ref_regex);
        // extract hint table
        std::size_t hint_tbl_offset = std::stoull(hint_tbl_ref_match[1]);
        std::size_t hint_tbl_size = std::stoull(hint_tbl_ref_match[2]);
        std::string hint_tbl_obj = doc_core.doc_contents.substr(hint_tbl_offset, hint_tbl_size);
        std::cout << hint_tbl_obj;
        std::cout << "\n\n" << doc_core.doc_contents[512] << doc_core.doc_contents[513] << doc_core.doc_contents[514];
    }

    void init_objects_root() {
        std::size_t root_object = 0;
        root_object = doc_core.object_refs[doc_core.ref_struct.root_object_ref.obj_num][doc_core.ref_struct.root_object_ref.gen_num].object_offset;
        doc_core.objects_root.object_contents = isolate_object_contents(doc_core.doc_contents, root_object); 
        std::size_t pages_obj = parse_obj_ref("/Pages", doc_core.objects_root.object_contents);
        doc_core.objects_root.pages = parse_obj_ref_array("/Kids", isolate_object_contents(doc_core.doc_contents, pages_obj));
        doc_core.objects_root.page_count = doc_core.objects_root.pages.size();
    }

// TODO: support linearisation

    int open(std::string path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return 1;

        std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        file.close();
        doc_core.doc_contents.clear();
        doc_core.doc_contents.assign(buffer.begin(), buffer.end());

        /* check if PDF is in linearised form, & if so, parse it according to its linearised structure */

        boost::regex linearisation_header_regex(R"(\d+\s+\d+\s+obj\s*<<(.*?/Linearized.*?)>>\s*endobj)");
        boost::smatch linearisation_header_match;
        if (boost::regex_search(doc_core.doc_contents, linearisation_header_match, linearisation_header_regex)) {
            std::cout << linearisation_header_match[1].str();
            prepare_linearised_pdf(linearisation_header_match[1]);
            return 0;
        }

        /* For normal PDF formats (comptible with version 1.5+)*/

        // Parse startxref
        boost::regex startxref_regex(R"(startxref\s*(\d+))");
        boost::smatch startxref_match;
        if (boost::regex_search(doc_core.doc_contents, startxref_match, startxref_regex)) {
            doc_core.ref_struct.startxref = std::stoull(startxref_match[1]);
        }
        
        // parse PDF's primary trailer ( if it has one )
        boost::regex trailer_regex(R"(trailer\s*<<([\s\S]*?)>>)");
        boost::smatch trailer_match;
        if (boost::regex_search(doc_core.doc_contents, trailer_match, trailer_regex)) { // if document has a trailer:
            parse_doc_trailer(trailer_match[1]);
            std::size_t xref_table_pos = doc_core.ref_struct.startxref;
            parse_xref_table(xref_table_pos);
            init_objects_root();
            return 0;
        }

        // parse PDF's xrefStream, decompressing any /ObjStm & generating the trailer from the compressed xref obj
        boost::regex xref_stm_regex(R"(\d+\s+\d+\s+obj\s*<<((?:(?!/Type).)*?/Type\s*/XRef.*?>>\s*stream[\s\S]*?endstream)\s*endobj)");
        boost::smatch xref_stm_match;
        if (boost::regex_search(doc_core.doc_contents, xref_stm_match, xref_stm_regex)) {
            parse_xref_stream(xref_stm_match[1]);
            std::size_t xref_table_pos = doc_core.ref_struct.startxref;
            parse_xref_table(xref_table_pos);
            init_objects_root();
            return 0;
        }

        return 1; // if none of the above checks worked, then the file being parsed is obviously malformed, exit with return code 1
    }


    int get_num_pages() {
        return doc_core.objects_root.page_count;
    }

    page get_page(int page_num) {
        return page(doc_core.objects_root.pages[page_num]);
    }

    page::page(std::size_t page_ref) {
        object_contents = isolate_object_contents(doc_core.doc_contents, page_ref);

        media_box = parse_rect("/MediaBox", object_contents);
        std::cout << media_box.bottom_left.x << media_box.bottom_left.y << media_box.top_right.x << media_box.top_right.y;
    
        std::size_t resources_pos = object_contents.find("/Resources"); // keep if needed in future

        // parse font objects for page
        font_refs = parse_obj_ref_dict("/Font", object_contents);
        x_obj_refs = parse_obj_ref_dict("/XObject", object_contents);
        check_x_obj_type(); // check which XObject type each mapped key represents & save them to a vector
        contents = parse_content_stream(parse_obj_ref("/Contents", object_contents));
    }

    rect page::get_media_box() {
        return media_box;
    }

    page::~page() {}
    
    page::pageContent page::parse_content_stream(std::size_t content_stream_ref) {
        pageContent contents;
        std::string object_contents = isolate_object_contents(doc_core.doc_contents, content_stream_ref);
        // decompress & save stream
        boost::regex stream_regex(R"(stream\s*\n((?:(?!endstream)[\s\S])*)\s*endstream)");
        boost::smatch stream_match;
        boost::regex_search(object_contents, stream_match, stream_regex);
        contents.stream = inflate_stream_to_str(stream_match[1]);
        std::cout << contents.stream;
        return contents;
    }

    // NOTE: add ability to also parse the graphics state properties given to text objects before the BT symbol

    std::vector<textObject> page::parse_text_objects() {
        std::vector<textObject> text_objs;
        boost::regex text_obj_regex(R"(BT(.*?)ET)");
        boost::sregex_iterator iter(contents.stream.begin(), contents.stream.end(), text_obj_regex);
        boost::sregex_iterator end;

        while (iter != end) {
            textObject obj;
            std::string obj_content = (*iter)[1].str();  // Capture the content between BT and ET
            // parse coords
            if (std::size_t pos = obj_content.find("Td"); pos != std::string::npos) {
                std::istringstream(obj_content.substr(obj_content.rfind("\n", pos) + 1, pos)) >> obj.text_coordinates.x >> obj.text_coordinates.y;
            }
            // find text blocks
            std::vector<std::size_t> text_block_positions;
            boost::regex text_block_regex(R"(/(\w+)\s*(\d+)\s*Tf\s*((?:\((.*?)\)Tj\s*)+))");
            boost::sregex_iterator text_iter(obj_content.begin(), obj_content.end(), text_block_regex);
            while (text_iter != end) {
                textData text_block;
                text_block.font = load_font((*text_iter)[1]);
                text_block.text_size = std::stoi((*text_iter)[2]);

                std::string text_content = (*text_iter)[3].str();
                boost::regex text_regex(R"(\((.*?)\)Tj)");
                boost::sregex_iterator text_content_iter(text_content.begin(), text_content.end(), text_regex);
                while (text_content_iter != end) {
                    text_block.text += (*text_content_iter)[1].str();
                    ++text_content_iter;
                }

                obj.text_blocks.push_back(text_block);
                ++text_iter;
            }
            text_objs.push_back(obj);
            ++iter;
        }
        return text_objs;
    }

    std::shared_ptr<fontObject> page::load_font(const std::string& font_key) {
        // check if font is not already loaded, if it is, return the font for use
        auto font_iter = font_cache.find(font_key);
        if (font_iter != font_cache.end()) {
            return font_iter->second;
        }
        // if font isn't loaded find its entry on the font ref map by the provided key
        auto font_ref_iter = font_refs.find(font_key);
        if (font_ref_iter == font_refs.end()) {
            throw std::runtime_error("No reference found for font object with key: " + font_key);
        }
        // parse font object & save to chache
        std::string obj_content = isolate_object_contents(doc_core.doc_contents, font_ref_iter->second);
        std::shared_ptr<fontObject> font = std::make_shared<fontObject>();
        font->font_name = get_tag_type(obj_content.find("/BaseFont"), obj_content);
        font->subtype = get_tag_value(obj_content.find("/Subtype"), obj_content);
        font_cache.emplace(font_key, font);
        return font;
    }

    void page::check_x_obj_type() {
        for (const auto& ref : x_obj_refs) {
            const std::string& key = ref.first;
            std::string x_obj_contents = isolate_object_contents(doc_core.doc_contents, ref.second);
            std::string type = get_tag_type(x_obj_contents.find("/Subtype", 0), x_obj_contents);
            if (type == "/Image") image_keys.push_back(key);
            if (type == "/Form") form_keys.push_back(key);
        }
    }

    std::vector<imageObject> page::parse_page_images() {
        std::vector<imageObject> imgs;
        boost::regex x_obj_regex(R"(q\s+([\s\S]*?)/(\w+)\s+Do\s+Q)"); // this gathers any calls for xObjects in the contents
        boost::smatch x_obj_match;
        std::string::const_iterator start = contents.stream.begin();
        std::string::const_iterator end = contents.stream.end();
        while (boost::regex_search(start, end, x_obj_match, x_obj_regex)) {
            std::string graphics_state = x_obj_match[1].str(); // Capture the graphics state info preceeding the '<key> Do' call
            std::string x_obj_key = x_obj_match[2].str(); // used key
            if (std::find(image_keys.begin(), image_keys.end(), x_obj_key) != image_keys.end()) { // if this succeeds the called XObject is an image
                imageObject img;
                std::string object_contents = isolate_object_contents(doc_core.doc_contents, x_obj_refs[x_obj_key]);
                // parse graphics state
                std::istringstream ctm_stream(graphics_state); 
                ctm_stream >> img.graphics_state.ctm.scale_x >> img.graphics_state.ctm.shear_y >> img.graphics_state.ctm.shear_x
                >> img.graphics_state.ctm.scale_y >> img.graphics_state.ctm.translate_x >> img.graphics_state.ctm.translate_y;
                // image width & height
                std::size_t width_pos = object_contents.find("/Width", 0);
                img.width = get_tag_value(width_pos, object_contents);
                std::size_t height_pos = object_contents.find("/Height", 0);
                img.height = get_tag_value(height_pos, object_contents);
                // extract bits per component
                std::size_t bits_per_component_pos = object_contents.find("/BitsPerComponent", 0);
                img.bits_per_component = get_tag_value(bits_per_component_pos, object_contents);
                // get colour space
                std::size_t colour_space_pos = object_contents.find("/ColorSpace", 0);
                std::string colour_space = get_tag_type(colour_space_pos, object_contents);
                if (colour_space == "/DeviceRGB") img.clr_space = DEVICE_RGB;
                if (colour_space == "/DeviceCMYK") img.clr_space = DEVICE_CMYK;
                // check interpolate bool value
                std::size_t interpolate_pos = object_contents.find("/Interpolate", 0);
                img.interpolate = get_tag_bool_value(interpolate_pos, object_contents);
                // decompress image stream
                boost::regex stream_regex(R"(stream\s*\n((?:(?!endstream)[\s\S])*)\s*endstream)");
                boost::smatch stream_match;
                boost::regex_search(object_contents, stream_match, stream_regex);
                img.image_stream = inflate_stream_to_raw(std::vector<uint8_t>(stream_match[1].begin(), stream_match[1].end()));
                imgs.push_back(img);
            }
            start = x_obj_match[0].second;
        }
        return imgs;
    }


std::vector<uint8_t> page::inflate_stream_to_raw(const std::vector<uint8_t>& deflated_stream) {
    z_stream stream{};
    int ret = inflateInit(&stream);

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
            inflateEnd(&stream);
            return {}; // Return an empty vector on error
        }

        inflated_stream.insert(inflated_stream.end(), buffer.data(), buffer.data() + chunk_size - stream.avail_out);
    }

    inflateEnd(&stream);

    return inflated_stream;
}

std::string page::inflate_stream_to_str(const std::string& deflated_stream) {
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

    inflateEnd(&zs);
    return decompressed_data;
}


}