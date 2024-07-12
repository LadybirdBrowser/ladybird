/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Utf8View.h>
#include <LibCore/ArgsParser.h>
#include <LibGfx/Font/OpenType/Hinting/Opcodes.h>
#include <LibGfx/Font/OpenType/Typeface.h>
#include <LibMain/Main.h>

using namespace OpenType::Hinting;

#define YELLOW "\e[33m"
#define CYAN "\e[36m"
#define PURPLE "\e[95m"
#define GREEN "\e[92m"
#define RESET "\e[0m"
#define GRAY "\e[90m"

struct InstructionPrinter : InstructionHandler {
    InstructionPrinter(bool enable_highlighting)
        : m_enable_highlighting(enable_highlighting)
    {
    }

    void before_operation(InstructionStream& stream, Opcode opcode) override
    {
        if (opcode == Opcode::FDEF && stream.current_position() > 1 && m_indent_level == 1)
            outln();
        switch (opcode) {
        case Opcode::EIF:
        case Opcode::ELSE:
        case Opcode::ENDF:
            m_indent_level--;
            break;
        default:
            break;
        }
        auto digits = int(AK::log10(float(stream.length()))) + 1;
        if (m_enable_highlighting)
            out(GRAY);
        out("{:0{}}:", stream.current_position() - 1, digits);
        if (m_enable_highlighting)
            out(RESET);
        out("{:{}}", ""sv, m_indent_level * 2);
    }

    void after_operation(InstructionStream&, Opcode opcode) override
    {
        switch (opcode) {
        case Opcode::IF:
        case Opcode::ELSE:
        case Opcode::IDEF:
        case Opcode::FDEF:
            m_indent_level++;
            break;
        default:
            break;
        }
    }

    void print_number(u16 value)
    {
        if (m_enable_highlighting)
            return out(GREEN " {}" RESET, value);
        return out(" {}", value);
    }

    void print_bytes(ReadonlyBytes bytes, bool first = true)
    {
        for (auto value : bytes) {
            if (!first)
                out(",");
            print_number(value);
            first = false;
        }
    }

    void print_words(ReadonlyBytes bytes, bool first = true)
    {
        for (size_t i = 0; i < bytes.size(); i += 2) {
            if (!first)
                out(",");
            print_number(bytes[i] << 8 | bytes[i + 1]);
            first = false;
        }
    }

    void default_handler(Context context) override
    {
        auto instruction = context.instruction();
        auto name = opcode_mnemonic(instruction.opcode());
        if (m_enable_highlighting)
            out(YELLOW);
        out(name);
        if (m_enable_highlighting)
            out(CYAN);
        out("[");
        if (m_enable_highlighting)
            out(PURPLE);
        if (instruction.flag_bits() > 0)
            out("{:0{}b}", to_underlying(instruction.opcode()) & ((1 << instruction.flag_bits()) - 1), instruction.flag_bits());
        if (m_enable_highlighting)
            out(CYAN);
        out("]");
        if (m_enable_highlighting)
            out(RESET);
        switch (instruction.opcode()) {
        case Opcode::NPUSHB... Opcode::NPUSHB_MAX:
            print_number(instruction.values().size());
            print_bytes(instruction.values(), false);
            break;
        case Opcode::NPUSHW... Opcode::NPUSHW_MAX:
            print_number(instruction.values().size() / 2);
            print_words(instruction.values(), false);
            break;
        case Opcode::PUSHB... Opcode::PUSHB_MAX:
            print_bytes(instruction.values());
            break;
        case Opcode::PUSHW... Opcode::PUSHW_MAX:
            print_words(instruction.values());
            break;
        default:
            break;
        }
        outln();
    }

private:
    bool m_enable_highlighting;
    u32 m_indent_level { 1 };
};

static bool s_disassembly_attempted = false;

static void print_disassembly(StringView name, Optional<ReadonlyBytes> program, bool enable_highlighting, u32 code_point = 0)
{
    s_disassembly_attempted = true;
    if (!program.has_value()) {
        out(name, code_point);
        outln(": not found");
        return;
    }
    out(name, code_point);
    outln(":    ({} bytes)\n", program->size());
    InstructionPrinter printer { enable_highlighting };
    InstructionStream stream { printer, *program };
    while (!stream.at_end())
        stream.process_next_instruction();
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    Core::ArgsParser args_parser;

    StringView font_path;
    bool no_color = false;
    bool dump_font_program = false;
    bool dump_prep_program = false;
    StringView text;
    args_parser.add_positional_argument(font_path, "Path to font", "FILE");
    args_parser.add_option(dump_font_program, "Disassemble font program (fpgm table)", "disasm-fpgm", 'f');
    args_parser.add_option(dump_prep_program, "Disassemble CVT program (prep table)", "disasm-prep", 'p');
    args_parser.add_option(text, "Disassemble glyph programs", "disasm-glyphs", 'g', "text");
    args_parser.add_option(no_color, "Disable syntax highlighting", "no-color", 'n');
    args_parser.parse(arguments);

    auto resource = TRY(Core::Resource::load_from_filesystem(font_path));
    auto font = TRY(OpenType::Typeface::try_load_from_resource(resource));

    if (dump_font_program)
        print_disassembly("Font program"sv, font->font_program(), !no_color);
    if (dump_prep_program) {
        if (dump_font_program)
            outln();
        print_disassembly("CVT program"sv, font->control_value_program(), !no_color);
    }
    if (!text.is_empty()) {
        Utf8View utf8_view { text };
        bool first = !(dump_font_program || dump_prep_program);
        for (u32 code_point : utf8_view) {
            if (!first)
                outln();
            auto glyph_id = font->glyph_id_for_code_point(code_point);
            print_disassembly("Glyph program for codepoint {}"sv, font->glyph_program(glyph_id), !no_color, code_point);
            first = false;
        }
    }

    if (!s_disassembly_attempted) {
        args_parser.print_usage(stderr, arguments.strings[0]);
        return 1;
    }
    return 0;
}
