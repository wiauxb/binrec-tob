#include "section_link.hpp"
#include "compiler_command.hpp"
#include "link_error.hpp"
#include <iostream>
#include <llvm/Support/Format.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

using namespace binrec;
using namespace llvm;
using namespace std;

auto binrec::link_recovered_binary(
    const vector<SectionInfo> &sections,
    std::string ld_script_path,
    std::string output_path,
    const vector<std::string> &input_paths,
    LinkContext &ctx) -> Error
{
    auto ld_script_template_buf_or_err = MemoryBuffer::getFile(ld_script_path);
    if (error_code ec = ld_script_template_buf_or_err.getError()) {
        return errorCodeToError(ec);
    }
    auto &ld_script_template_buf = ld_script_template_buf_or_err.get();
    string ld_script_template{
        ld_script_template_buf->getBufferStart(),
        ld_script_template_buf->getBufferEnd()};

    string sections_buf;
    raw_string_ostream sections_out{sections_buf};
    for (const SectionInfo &section : sections) {
        sections_out << formatv("  . = {0:x+8} ;\n", section.virtual_address);
        //        if (section.name == ".dynamic") {
        //            sections_out << "  .dynamic : { *(.orig.dynamic) } :orig :dynamic\n";
        //        } else if (section.name == ".interp") {
        //            sections_out << "  .interp : { *(.orig.interp) } :orig :interp\n";
        //        } else {
        sections_out << formatv("  .orig{0} : { *(.orig{0}) } :orig\n", section.name);
        //}
    }
    sections_out.flush();

    string placeholder_sections_str = "PLACEHOLDER_SECTIONS";
    auto placeholder_sections_begin = ld_script_template.find(placeholder_sections_str);
    LINK_ASSERT(placeholder_sections_begin != string::npos);
    ld_script_template.replace(
        placeholder_sections_begin,
        placeholder_sections_str.size(),
        sections_buf);

    SmallString<128> linker_script_filename = ctx.work_dir;
    linker_script_filename.append("/script.ld");
    error_code ec;
    raw_fd_ostream linker_script(linker_script_filename, ec);
    if (ec) {
        return errorCodeToError(ec);
    }
    linker_script << ld_script_template;
    linker_script.close();

    CompilerCommand cc{CompilerCommandMode::Link};
    cc.linker_script_path = linker_script_filename.c_str();
    cc.output_path = output_path;
    cc.input_paths = input_paths;
    cc.harden = ctx.harden;

    ec = cc.run();
    if (ec) {
        return errorCodeToError(ec);
    }

    auto binary_or_error = object::createBinary(output_path);
    if (auto err = binary_or_error.takeError()) {
        return err;
    }
    ctx.recovered_binary = move(binary_or_error.get());

    return Error::success();
}
