#!/usr/bin/env python3
"""Generate the munx Language Specification PDF."""

from __future__ import annotations

from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_JUSTIFY, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import cm, mm
from reportlab.platypus import (
    KeepTogether,
    ListFlowable,
    ListItem,
    PageBreak,
    Paragraph,
    Preformatted,
    SimpleDocTemplate,
    Spacer,
    Table,
    TableStyle,
)

OUT = Path(__file__).resolve().parent / "munx-language-specification.pdf"

INK = colors.HexColor("#1a1a1a")
MUTED = colors.HexColor("#444444")
RULE = colors.HexColor("#c8c8c8")
CODE_BG = colors.HexColor("#f4f4f1")
ACCENT = colors.HexColor("#0b3d2e")
HEADER_BG = colors.HexColor("#e8efe9")


def styles():
    """Build the named ParagraphStyle map used by the PDF."""
    base = getSampleStyleSheet()
    s = {
        "cover_title": ParagraphStyle(
            "cover_title",
            parent=base["Title"],
            fontName="Helvetica-Bold",
            fontSize=28,
            leading=34,
            textColor=ACCENT,
            alignment=TA_CENTER,
            spaceAfter=12,
        ),
        "cover_sub": ParagraphStyle(
            "cover_sub",
            parent=base["Normal"],
            fontName="Helvetica",
            fontSize=12,
            leading=16,
            textColor=MUTED,
            alignment=TA_CENTER,
            spaceAfter=6,
        ),
        "h1": ParagraphStyle(
            "h1",
            parent=base["Heading1"],
            fontName="Helvetica-Bold",
            fontSize=16,
            leading=20,
            textColor=ACCENT,
            spaceBefore=18,
            spaceAfter=8,
            borderPadding=3,
        ),
        "h2": ParagraphStyle(
            "h2",
            parent=base["Heading2"],
            fontName="Helvetica-Bold",
            fontSize=12,
            leading=15,
            textColor=INK,
            spaceBefore=12,
            spaceAfter=6,
        ),
        "h3": ParagraphStyle(
            "h3",
            parent=base["Heading3"],
            fontName="Helvetica-Bold",
            fontSize=10.5,
            leading=13,
            textColor=INK,
            spaceBefore=8,
            spaceAfter=4,
        ),
        "body": ParagraphStyle(
            "body",
            parent=base["Normal"],
            fontName="Helvetica",
            fontSize=9.5,
            leading=13,
            textColor=INK,
            alignment=TA_JUSTIFY,
            spaceAfter=6,
        ),
        "bullet": ParagraphStyle(
            "bullet",
            parent=base["Normal"],
            fontName="Helvetica",
            fontSize=9.5,
            leading=12.5,
            textColor=INK,
            leftIndent=8,
        ),
        "code": ParagraphStyle(
            "code",
            parent=base["Code"],
            fontName="Courier",
            fontSize=8,
            leading=10.5,
            textColor=INK,
            backColor=CODE_BG,
            leftIndent=4,
            rightIndent=4,
            spaceBefore=4,
            spaceAfter=8,
        ),
        "caption": ParagraphStyle(
            "caption",
            parent=base["Normal"],
            fontName="Helvetica-Oblique",
            fontSize=8,
            leading=10,
            textColor=MUTED,
            alignment=TA_CENTER,
            spaceAfter=10,
        ),
        "toc": ParagraphStyle(
            "toc",
            parent=base["Normal"],
            fontName="Helvetica",
            fontSize=10,
            leading=16,
            textColor=INK,
            leftIndent=10,
        ),
        "footer": ParagraphStyle(
            "footer",
            parent=base["Normal"],
            fontName="Helvetica",
            fontSize=8,
            textColor=MUTED,
            alignment=TA_CENTER,
        ),
        "table_cell": ParagraphStyle(
            "table_cell",
            parent=base["Normal"],
            fontName="Helvetica",
            fontSize=8,
            leading=10.5,
            textColor=INK,
        ),
        "table_head": ParagraphStyle(
            "table_head",
            parent=base["Normal"],
            fontName="Helvetica-Bold",
            fontSize=8,
            leading=10.5,
            textColor=INK,
        ),
    }
    return s


def P(text: str, style):
    """Create a Paragraph, converting newlines to ``<br/>``."""
    return Paragraph(text.replace("\n", "<br/>"), style)


def code_block(text: str, st):
    """Wrap munx source in a shaded Preformatted table cell."""
    # Preformatted preserves spaces; wrap in a tiny table for background.
    pre = Preformatted(text.rstrip() + "\n", st["code"])
    t = Table([[pre]], colWidths=[16.5 * cm])
    t.setStyle(
        TableStyle(
            [
                ("BACKGROUND", (0, 0), (-1, -1), CODE_BG),
                ("BOX", (0, 0), (-1, -1), 0.4, RULE),
                ("LEFTPADDING", (0, 0), (-1, -1), 6),
                ("RIGHTPADDING", (0, 0), (-1, -1), 6),
                ("TOPPADDING", (0, 0), (-1, -1), 4),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
            ]
        )
    )
    return t


def make_table(headers, rows, st, col_widths=None):
    """Build a styled ReportLab table from string headers/rows."""
    data = [[P(h, st["table_head"]) for h in headers]]
    for row in rows:
        data.append([P(c, st["table_cell"]) for c in row])
    t = Table(data, colWidths=col_widths, repeatRows=1)
    t.setStyle(
        TableStyle(
            [
                ("BACKGROUND", (0, 0), (-1, 0), HEADER_BG),
                ("TEXTCOLOR", (0, 0), (-1, -1), INK),
                ("GRID", (0, 0), (-1, -1), 0.3, RULE),
                ("VALIGN", (0, 0), (-1, -1), "TOP"),
                ("LEFTPADDING", (0, 0), (-1, -1), 4),
                ("RIGHTPADDING", (0, 0), (-1, -1), 4),
                ("TOPPADDING", (0, 0), (-1, -1), 3),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 3),
            ]
        )
    )
    return t


def bullets(items, st):
    """Create a bulleted ListFlowable from string items."""
    return ListFlowable(
        [ListItem(P(i, st["bullet"]), leftIndent=12, bulletColor=ACCENT) for i in items],
        bulletType="bullet",
        start="•",
    )


def footer(canvas, doc):
    """Draw the page footer (title + page number) on each page."""
    canvas.saveState()
    canvas.setStrokeColor(RULE)
    canvas.setLineWidth(0.4)
    y = 1.4 * cm
    canvas.line(2 * cm, y + 4, A4[0] - 2 * cm, y + 4)
    canvas.setFont("Helvetica", 8)
    canvas.setFillColor(MUTED)
    canvas.drawString(2 * cm, y - 6, "munx Language Specification")
    canvas.drawRightString(A4[0] - 2 * cm, y - 6, f"{doc.page}")
    canvas.restoreState()


def build():
    """Assemble the full specification and write it to ``OUT``."""
    st = styles()
    doc = SimpleDocTemplate(
        str(OUT),
        pagesize=A4,
        leftMargin=2 * cm,
        rightMargin=2 * cm,
        topMargin=1.8 * cm,
        bottomMargin=2 * cm,
        title="munx Language Specification",
        author="munx project",
        subject="Formal specification of the munx programming language",
    )

    story = []

    # ----- Cover -----
    story.append(Spacer(1, 4.5 * cm))
    story.append(P("munx", st["cover_title"]))
    story.append(P("Language Specification", st["cover_title"]))
    story.append(Spacer(1, 0.6 * cm))
    story.append(P("A systems language with first-class concurrency and I/O", st["cover_sub"]))
    story.append(P("Version 0.1 &mdash; derived from the reference front-end in this repository", st["cover_sub"]))
    story.append(P("Source grammar: <font face='Courier'>sample/**/*.mx</font>", st["cover_sub"]))
    story.append(Spacer(1, 2 * cm))
    story.append(
        P(
            "This document describes the lexical structure, types, expressions, "
            "statements, modules, concurrency primitives, and abstract syntax of "
            "<b>munx</b>. The normative grammar is the behaviour of the reference "
            "lexer and parser (<font face='Courier'>include/lexer.hpp</font>, "
            "<font face='Courier'>include/parser.hpp</font>).",
            st["body"],
        )
    )
    story.append(PageBreak())

    # ----- TOC -----
    story.append(P("Contents", st["h1"]))
    toc_items = [
        "1. Introduction",
        "2. Lexical structure",
        "3. Types",
        "4. Expressions",
        "5. Statements and declarations",
        "6. Modules and packages",
        "7. Concurrency and I/O",
        "8. Memory management",
        "9. Standard builtins (prelude)",
        "10. Abstract syntax tree",
        "11. Operator precedence",
        "12. Grammar sketch",
        "13. Example programs",
        "14. Implementation notes",
    ]
    for item in toc_items:
        story.append(P(item, st["toc"]))
    story.append(PageBreak())

    # ----- 1 -----
    story.append(P("1. Introduction", st["h1"]))
    story.append(
        P(
            "<b>munx</b> is a systems-oriented programming language. Programs are "
            "stored in <font face='Courier'>.mx</font> source files. The language "
            "emphasises explicit concurrency (threads, locks, pipes, join) and "
            "I/O (sockets, files, terminals) as ordinary values rather than "
            "hidden runtime magic.",
            st["body"],
        )
    )
    story.append(
        P(
            "The reference implementation in this repository is a front-end only: "
            "it lexes and parses source into an AST. Bytecode generation and VM "
            "execution are out of scope for the front-end, but opcodes are sketched "
            "in <font face='Courier'>include/Opcode.hpp</font>.",
            st["body"],
        )
    )
    story.append(P("1.1 Design principles", st["h2"]))
    story.append(
        bullets(
            [
                "Sample-driven grammar — accepted syntax is whatever <font face='Courier'>sample/</font> uses.",
                "Structural keywords are few; most builtins remain ordinary identifiers so they can be rebound (e.g. <font face='Courier'>print = fix(...)</font>).",
                "Semicolons are optional statement terminators.",
                "Blocks are brace-delimited; indentation is not significant.",
                "Only enums may be matched with <font face='Courier'>match</font>.",
                "Casts apply only between primitive types.",
            ],
            st,
        )
    )

    # ----- 2 -----
    story.append(P("2. Lexical structure", st["h1"]))
    story.append(P("2.1 Encoding and whitespace", st["h2"]))
    story.append(
        P(
            "Source is a sequence of Unicode characters treated as bytes by the "
            "reference lexer. Whitespace (space, tab, CR, LF) separates tokens and "
            "is otherwise insignificant.",
            st["body"],
        )
    )
    story.append(P("2.2 Comments", st["h2"]))
    story.append(
        bullets(
            [
                "Line comments: <font face='Courier'>//</font> to end of line.",
                "Block comments: <font face='Courier'>/* ... */</font>, non-nesting. An unclosed block comment is a compile error.",
            ],
            st,
        )
    )
    story.append(P("2.3 Identifiers", st["h2"]))
    story.append(
        P(
            "An identifier starts with a letter or <font face='Courier'>_</font> and "
            "continues with letters, digits, or <font face='Courier'>_</font>. "
            "Identifiers are case-sensitive. The single underscore "
            "<font face='Courier'>_</font> is a discard target in destructuring.",
            st["body"],
        )
    )
    story.append(P("2.4 Keywords", st["h2"]))
    story.append(
        P(
            "The following words are tokenized as <font face='Courier'>KEYWORD</font>. "
            "Elsewhere they may still appear where an identifier is expected "
            "(names, members, enum members), except that "
            "<font face='Courier'>load_package</font> / "
            "<font face='Courier'>load_packages</font> are rejected outside the "
            "import header.",
            st["body"],
        )
    )
    story.append(
        code_block(
            "package load_package load_packages\n"
            "func lambda return if else loop break\n"
            "match case enum object tuple\n"
            "alloc delete free\n"
            "lock acquire release join\n"
            "monitor trap cast fail",
            st,
        )
    )
    story.append(P("2.5 Literals", st["h2"]))
    story.append(
        make_table(
            ["Kind", "Form", "Notes"],
            [
                ["Integer", "<font face='Courier'>0</font>, <font face='Courier'>42</font>, <font face='Courier'>007</font>", "Signed 64-bit. Overflow is a compile error."],
                ["Float", "<font face='Courier'>3.14</font>, <font face='Courier'>0.5</font>", "Digits required on both sides of <font face='Courier'>.</font>."],
                ["String", '<font face="Courier">"..."</font>', "Escapes: <font face='Courier'>\\n \\t \\a \\\\ \\\" \\'</font>."],
                ["Character", "<font face='Courier'>'a'</font>, <font face='Courier'>'\\n'</font>", "Exactly one character (after escapes)."],
                ["Boolean", "<font face='Courier'>true</font>, <font face='Courier'>false</font>", "Literals, not keywords."],
                ["Null", "<font face='Courier'>null</font>", "Absence of a value (e.g. closed stream)."],
                ["Regex", '<font face="Courier">r"..."</font>', "Raw pattern; no escape processing; cannot contain <font face='Courier'>\"</font>."],
            ],
            st,
            col_widths=[2.8 * cm, 4.5 * cm, 9.2 * cm],
        )
    )
    story.append(Spacer(1, 4 * mm))
    story.append(P("2.6 Operators and punctuation", st["h2"]))
    story.append(
        code_block(
            "( ) { } [ ] , : ; . ::\n"
            "= += == != < > <= >=\n"
            "+ - * / % ! ~ && ||\n"
            "=>   ->   <-",
            st,
        )
    )
    story.append(
        P(
            "<font face='Courier'>-&gt;</font> inserts into a named pipe; "
            "<font face='Courier'>&lt;-</font> extracts from a named pipe (blocking). "
            "<font face='Courier'>=&gt;</font> introduces a lambda body. "
            "The tokens <font face='Courier'>&amp;</font>, <font face='Courier'>|</font>, "
            "and <font face='Courier'>^</font> are recognised by the lexer but have "
            "no parser production (bitwise binary ops are currently unsupported).",
            st["body"],
        )
    )

    # ----- 3 -----
    story.append(P("3. Types", st["h1"]))
    story.append(
        P(
            "Types appear in parameter lists, return annotations, object fields, "
            "cast targets, trap parameters, and typed array literals. Expression "
            "nodes in the AST are untyped; inference is left to later stages.",
            st["body"],
        )
    )
    story.append(P("3.1 Type forms", st["h2"]))
    story.append(
        make_table(
            ["Form", "Syntax", "Meaning"],
            [
                ["Primitive", "<font face='Courier'>int</font>, <font face='Courier'>file</font>, …", "Built-in scalar or I/O type."],
                ["Named", "<font face='Courier'>Person</font>", "User-defined enum or object name."],
                ["Array", "<font face='Courier'>[T]</font>", "Homogeneous dynamic array of <font face='Courier'>T</font>."],
                ["Tuple", "<font face='Courier'>tuple[T1, T2, …]</font>", "Fixed product type; may be empty: <font face='Courier'>tuple[]</font>."],
            ],
            st,
            col_widths=[2.8 * cm, 5.5 * cm, 8.2 * cm],
        )
    )
    story.append(Spacer(1, 4 * mm))
    story.append(P("3.2 Primitive types", st["h2"]))
    story.append(
        make_table(
            ["Name", "Role"],
            [
                ["<font face='Courier'>int</font>", "Signed 64-bit integer."],
                ["<font face='Courier'>float</font>", "Floating-point number."],
                ["<font face='Courier'>bool</font>", "Boolean; used in conditions."],
                ["<font face='Courier'>string</font>", "UTF-8 text (implementation-defined encoding)."],
                ["<font face='Courier'>character</font>", "Single character value."],
                ["<font face='Courier'>void</font>", "No value; only as a return type."],
                ["<font face='Courier'>socket</font>", "Network socket from <font face='Courier'>open(io_type::socket, …)</font>."],
                ["<font face='Courier'>file</font>", "Filesystem handle from <font face='Courier'>open(io_type::file, …)</font>."],
                ["<font face='Courier'>term</font>", "Terminal/TTY handle from <font face='Courier'>open(io_type::tty, …)</font>."],
                ["<font face='Courier'>exception</font>", "Error value delivered to a <font face='Courier'>trap</font> handler."],
            ],
            st,
            col_widths=[3.2 * cm, 13.3 * cm],
        )
    )
    story.append(Spacer(1, 3 * mm))
    story.append(
        P(
            "The three I/O object types are selected by the first argument to "
            "<font face='Courier'>open</font>:",
            st["body"],
        )
    )
    story.append(
        make_table(
            ["open kind", "Result type"],
            [
                ["<font face='Courier'>io_type::socket</font>", "<font face='Courier'>socket</font>"],
                ["<font face='Courier'>io_type::file</font>", "<font face='Courier'>file</font>"],
                ["<font face='Courier'>io_type::tty</font>", "<font face='Courier'>term</font>"],
            ],
            st,
            col_widths=[5 * cm, 5 * cm],
        )
    )
    story.append(Spacer(1, 3 * mm))
    story.append(
        P(
            "Enum members such as <font face='Courier'>io_type::file</font> are "
            "ordinary enum-access expressions. Only type <i>positions</i> "
            "(parameters, fields, casts, traps) resolve the name "
            "<font face='Courier'>file</font> to the primitive.",
            st["body"],
        )
    )
    story.append(P("3.3 Casts", st["h2"]))
    story.append(
        P(
            "Syntax: <font face='Courier'>cast[T](expr)</font>. Casts are defined "
            "only for primitive types. Converting compound or user types must be "
            "implemented manually.",
            st["body"],
        )
    )
    story.append(
        code_block(
            'i = cast[int]("134")\n'
            "s = cast[string](i)          // \"134\"\n"
            'cast[string]([int][1,2,3])   // "[1,2,3]"',
            st,
        )
    )

    # ----- 4 -----
    story.append(P("4. Expressions", st["h1"]))
    story.append(P("4.1 Primary expressions", st["h2"]))
    story.append(
        bullets(
            [
                "Literals (see §2.5)",
                "Identifiers and <font face='Courier'>Enum::Member</font> access",
                "Grouped expressions <font face='Courier'>(e)</font>",
                "Array literals <font face='Courier'>[e, …]</font> and typed arrays <font face='Courier'>[T][e, …]</font>",
                "Tuple literals <font face='Courier'>{e, …}</font> (including <font face='Courier'>{}</font>)",
                "Casts, <font face='Courier'>alloc</font>, <font face='Courier'>delete</font> / <font face='Courier'>free</font>, lambdas",
            ],
            st,
        )
    )
    story.append(P("4.2 Postfix", st["h2"]))
    story.append(
        bullets(
            [
                "Member access: <font face='Courier'>obj.field</font>",
                "Call: <font face='Courier'>f(a, b)</font>",
                "Index: <font face='Courier'>items[i]</font> (single index expression)",
            ],
            st,
        )
    )
    story.append(P("4.3 Unary operators", st["h2"]))
    story.append(
        code_block("!expr    // logical not\n~expr    // bitwise not\n-expr    // negation\n<- name  // blocking pipe extract", st)
    )
    story.append(P("4.4 Binary operators", st["h2"]))
    story.append(
        code_block(
            "* / %          // multiplicative\n"
            "+ -            // additive\n"
            "< > <= >=      // relational\n"
            "== !=          // equality\n"
            "&&             // logical and\n"
            "||             // logical or\n"
            "expr -> name   // pipe insert (right side is a pipe name)",
            st,
        )
    )
    story.append(P("4.5 Lambdas", st["h2"]))
    story.append(
        code_block(
            "f = lambda (x: int): int => {\n"
            "    return x * 2\n"
            "}\n"
            "\n"
            "// immediately invoked\n"
            "result = lambda (x: int): int => { return x + 1 }(41)",
            st,
        )
    )
    story.append(
        P(
            "A lambda always has a parameter list, a return type, <font face='Courier'>=&gt;</font>, "
            "and a block body. Unlike <font face='Courier'>func</font>, the arrow is required.",
            st["body"],
        )
    )

    # ----- 5 -----
    story.append(P("5. Statements and declarations", st["h1"]))
    story.append(P("5.1 Assignment and destructuring", st["h2"]))
    story.append(
        P(
            "An assignment target is always a plain name (or <font face='Courier'>_</font>). "
            "Member and index expressions cannot appear on the left-hand side.",
            st["body"],
        )
    )
    story.append(
        code_block(
            "x = 1\n"
            "x += 2\n"
            "a, b = divide(9, 2)\n"
            "{data, addr} = read(server, 1024)\n"
            "{resp, _} = read(client, 1024)",
            st,
        )
    )
    story.append(P("5.2 Control flow", st["h2"]))
    story.append(
        code_block(
            "if cond {\n"
            "    ...\n"
            "} else if other {\n"
            "    ...\n"
            "} else {\n"
            "    ...\n"
            "}\n"
            "\n"
            "loop cond { ... }     // conditional\n"
            "loop { ... break }    // infinite until break\n"
            "\n"
            "match value {\n"
            "    case Color::Red { ... }\n"
            "    case Color::Blue { ... }\n"
            "}",
            st,
        )
    )
    story.append(
        P(
            "<font face='Courier'>match</font> scrutinees must be enums. Each "
            "<font face='Courier'>case</font> names <font face='Courier'>Enum::Member</font> "
            "and a block. Zero or more cases are allowed.",
            st["body"],
        )
    )
    story.append(P("5.3 Functions", st["h2"]))
    story.append(
        code_block(
            "func double(param: int): int {\n"
            "    return param * 2\n"
            "}\n"
            "\n"
            "func divide(a: int, b: int): tuple[int, int] {\n"
            "    return {a / b, a % b}\n"
            "}",
            st,
        )
    )
    story.append(
        P(
            "Every function has a return type annotation. Parameters are "
            "<font face='Courier'>name: type</font>. Nested function, enum, and "
            "object declarations are permitted inside blocks.",
            st["body"],
        )
    )
    story.append(P("5.4 Enums and objects", st["h2"]))
    story.append(
        code_block(
            "enum Gender { Male, Female }\n"
            "\n"
            "object Person {\n"
            "    name: string,\n"
            "    age: int,\n"
            "    gender: Gender\n"
            "}\n"
            "\n"
            "person = Person(\"Ada\", 36, Gender::Female)",
            st,
        )
    )
    story.append(
        P(
            "Object construction is an ordinary call whose callee is the type name. "
            "Trailing commas in enum or object member lists are not allowed.",
            st["body"],
        )
    )
    story.append(P("5.5 Exception handling", st["h2"]))
    story.append(
        code_block(
            "monitor {\n"
            "    risky()\n"
            "} trap(err: exception) {\n"
            "    println(err)\n"
            "}",
            st,
        )
    )
    story.append(
        P(
            "A <font face='Courier'>monitor</font> block is always followed by a "
            "<font face='Courier'>trap(name: type)</font> handler block. "
            "<font face='Courier'>fail(message)</font> aborts with an error.",
            st["body"],
        )
    )
    story.append(P("5.6 Blocks and layout", st["h2"]))
    story.append(
        P(
            "A bare <font face='Courier'>{ ... }</font> is a block statement. "
            "Semicolons after statements are optional. A tuple literal cannot begin "
            "a statement, because <font face='Courier'>{</font> opens a block — "
            "bind the tuple to a name first.",
            st["body"],
        )
    )

    # ----- 6 -----
    story.append(P("6. Modules and packages", st["h1"]))
    story.append(
        P(
            "Every source file begins with a package declaration, optionally "
            "followed by an import header:",
            st["body"],
        )
    )
    story.append(
        code_block(
            "package chatrelay\n"
            "load_package types\n"
            "load_packages {logger, server}\n"
            "\n"
            "// first non-import statement ends the header\n"
            "x = 1",
            st,
        )
    )
    story.append(
        bullets(
            [
                "<font face='Courier'>load_package name</font> — import one package.",
                "<font face='Courier'>load_packages {a, b, …}</font> — import several; the list must be non-empty and must not end with a trailing comma.",
                "Both forms may repeat and interleave, but only before the first statement.",
                "An import after a declaration, or inside any block/function/lambda/branch, is a compile error.",
                "The package declaration itself must be the first token of the file.",
            ],
            st,
        )
    )

    # ----- 7 -----
    story.append(P("7. Concurrency and I/O", st["h1"]))
    story.append(P("7.1 Threads and join", st["h2"]))
    story.append(
        code_block(
            "t = thread(lambda (n: int): void => {\n"
            "    print(n)\n"
            "}, {1})\n"
            "join [t]\n"
            "join [t1, t2, t3]   // names only; may be empty: join []",
            st,
        )
    )
    story.append(P("7.2 Locks", st["h2"]))
    story.append(
        code_block(
            "lock counter_lock\n"
            "acquire counter_lock\n"
            "counter += 1\n"
            "release counter_lock",
            st,
        )
    )
    story.append(P("7.3 Pipes", st["h2"]))
    story.append(
        P(
            "Pipes are named channels. Creation is an ordinary call; insertion and "
            "extraction use dedicated operators. The pipe operand of "
            "<font face='Courier'>-&gt;</font> / <font face='Courier'>&lt;-</font> "
            "must be a bare identifier (not an expression).",
            st["body"],
        )
    )
    story.append(
        code_block(
            'out_pipe = pipe("id", out)\n'
            "42 -> out_pipe\n"
            "value = <- out_pipe   // blocks until a value is available",
            st,
        )
    )
    story.append(P("7.4 Sockets, files, and terminals", st["h2"]))
    story.append(
        code_block(
            "server = open(io_type::socket, af_type::inet, sock_type::stream)\n"
            'bind(server, "127.0.0.1", 8000)\n'
            "listen(server, 128)\n"
            "client, addr = accept(server)\n"
            "data = read(client, 1024)\n"
            'write(client, "ok")\n'
            "close(client)\n"
            "\n"
            'f = open(io_type::file, "/tmp/log.txt", out)\n'
            "print = fix(open(io_type::tty, out))",
            st,
        )
    )
    story.append(
        P(
            "<font face='Courier'>fix(expr)</font> binds a reference and prevents "
            "reassignment — used for global terminal handles such as "
            "<font face='Courier'>print</font>, <font face='Courier'>println</font>, "
            "and <font face='Courier'>readln</font>.",
            st["body"],
        )
    )

    # ----- 8 -----
    story.append(P("8. Memory management", st["h1"]))
    story.append(
        code_block(
            "buff = alloc [100] [1, 2, 3]   // capacity, then initial values\n"
            "empty = alloc [64] []\n"
            "delete buff\n"
            "free empty                    // alias of delete",
            st,
        )
    )
    story.append(
        P(
            "Both the capacity brackets and the initializer brackets are required. "
            "<font face='Courier'>delete</font> / <font face='Courier'>free</font> "
            "take a buffer name.",
            st["body"],
        )
    )

    # ----- 9 -----
    story.append(P("9. Standard builtins (prelude)", st["h1"]))
    story.append(
        P(
            "Builtins are ordinary identifiers resolved by the runtime, not "
            "structural keywords. The samples establish the following surface "
            "(non-exhaustive):",
            st["body"],
        )
    )
    story.append(
        make_table(
            ["Area", "Names"],
            [
                ["I/O", "<font face='Courier'>open bind listen accept connect read write close</font>"],
                ["Terminal", "<font face='Courier'>print println readln fix in out</font>"],
                ["Concurrency", "<font face='Courier'>thread pipe queue push pop sleep process</font>"],
                ["Strings / arrays", "<font face='Courier'>concat len split trim append remove_at has_substring has_substring_regex</font>"],
                ["Control", "<font face='Courier'>argv</font> (implicit argument array)"],
            ],
            st,
            col_widths=[3.2 * cm, 13.3 * cm],
        )
    )

    # ----- 10 -----
    story.append(P("10. Abstract syntax tree", st["h1"]))
    story.append(
        P(
            "Nodes use an explicit discriminator plus a <font face='Courier'>std::variant</font> "
            "payload (<font face='Courier'>include/ast.hpp</font>). Helpers "
            "<font face='Courier'>make_expr_ptr</font>, <font face='Courier'>make_stmt_ptr</font>, "
            "<font face='Courier'>as&lt;T&gt;</font>, and <font face='Courier'>as_stmt&lt;T&gt;</font> "
            "keep the discriminator aligned with <font face='Courier'>value.index()</font>.",
            st["body"],
        )
    )
    story.append(P("10.1 Root", st["h2"]))
    story.append(
        code_block(
            "struct program {\n"
            "    string package_name;\n"
            "    vector&lt;load_package_stmt&gt; imports;\n"
            "    vector&lt;unique_ptr&lt;stmt_node&gt;&gt; statements;\n"
            "};",
            st,
        )
    )
    story.append(P("10.2 Expression kinds (<font face='Courier'>expr_type</font>)", st["h2"]))
    story.append(
        P(
            "IntLiteral · FloatLiteral · StringLiteral · CharLiteral · BoolLiteral · "
            "NullLiteral · RegexLiteral · Identifier · Binary · Unary · Call · "
            "Member · EnumAccess · Index · ArrayLiteral · TypedArrayLiteral · "
            "TupleLiteral · PipeInsert · PipeExtract · Cast · Alloc · Free · Lambda",
            st["body"],
        )
    )
    story.append(P("10.3 Statement kinds (<font face='Courier'>stmt_type</font>)", st["h2"]))
    story.append(
        P(
            "Assignment · Expr · Return · Break · Block · If · Loop · Match · "
            "FuncDecl · EnumDecl · ObjectDecl · Monitor · Lock · Acquire · "
            "Release · LoadPackage",
            st["body"],
        )
    )
    story.append(P("10.4 Type kinds (<font face='Courier'>type_kind</font>)", st["h2"]))
    story.append(
        P(
            "Primitive · Named · Array · Tuple — with "
            "<font face='Courier'>primitive_kind</font> values Int, Float, Bool, "
            "String, Character, Void, Socket, File, Term, Exception.",
            st["body"],
        )
    )

    # ----- 11 -----
    story.append(P("11. Operator precedence", st["h1"]))
    story.append(
        P("From tightest to loosest binding:", st["body"]),
    )
    story.append(
        make_table(
            ["Level", "Operators", "Associativity"],
            [
                ["1 Postfix", "<font face='Courier'>.  ()  []</font>", "Left"],
                ["2 Unary", "<font face='Courier'>!  ~  -  &lt;-</font>", "Right"],
                ["3 Multiplicative", "<font face='Courier'>*  /  %</font>", "Left"],
                ["4 Additive", "<font face='Courier'>+  -</font>", "Left"],
                ["5 Relational", "<font face='Courier'>&lt;  &gt;  &lt;=  &gt;=</font>", "Left"],
                ["6 Equality", "<font face='Courier'>==  !=</font>", "Left"],
                ["7 Logical and", "<font face='Courier'>&amp;&amp;</font>", "Left"],
                ["8 Logical or", "<font face='Courier'>||</font>", "Left"],
                ["9 Pipe insert", "<font face='Courier'>-&gt;</font>", "None (suffix)"],
                ["10 Assignment", "<font face='Courier'>=  +=</font>", "Statement"],
            ],
            st,
            col_widths=[3.5 * cm, 7 * cm, 6 * cm],
        )
    )

    # ----- 12 -----
    story.append(P("12. Grammar sketch", st["h1"]))
    story.append(
        P(
            "Informal EBNF reflecting the reference parser. "
            "<font face='Courier'>name</font> is an identifier or keyword used as a name.",
            st["body"],
        )
    )
    story.append(
        code_block(
            "program     = 'package' name { import } { statement } ;\n"
            "import      = 'load_package' name\n"
            "            | 'load_packages' '{' name { ',' name } '}' ;\n"
            "\n"
            "statement   = func_decl | enum_decl | object_decl\n"
            "            | if_stmt | loop_stmt | match_stmt | monitor_stmt\n"
            "            | 'lock' name | 'acquire' name | 'release' name\n"
            "            | 'return' [ expr ] | 'break' | 'join' '[' [ name { ',' name } ] ']'\n"
            "            | assignment | expr | block ;\n"
            "\n"
            "assignment  = targets ( '=' | '+=' ) expr ;\n"
            "targets     = name { ',' name }\n"
            "            | '{' name { ',' name } '}' ;\n"
            "\n"
            "type        = 'tuple' '[' [ type { ',' type } ] ']'\n"
            "            | '[' type ']'\n"
            "            | name ;\n"
            "\n"
            "expr        = pipe ;   /* see precedence table */\n"
            "lambda      = 'lambda' '(' params ')' ':' type '=>' block ;\n"
            "block       = '{' { statement } '}' ;",
            st,
        )
    )

    # ----- 13 -----
    story.append(P("13. Example programs", st["h1"]))
    story.append(P("13.1 Minimal package", st["h2"]))
    story.append(
        code_block(
            "package hello\n"
            'println = fix(open(io_type::tty, out))\n'
            'println("hello, munx")',
            st,
        )
    )
    story.append(P("13.2 Function, match, and pipes", st["h2"]))
    story.append(
        code_block(
            "package demo\n"
            "load_package types\n"
            "\n"
            "enum Color { Red, Green, Blue }\n"
            "\n"
            "func paint(c: Color): void {\n"
            "    match c {\n"
            "        case Color::Red { print(\"red\") }\n"
            "        case Color::Green { print(\"green\") }\n"
            "        case Color::Blue { print(\"blue\") }\n"
            "    }\n"
            "}\n"
            "\n"
            'p = pipe("ready", out)\n'
            "1 -> p\n"
            "flag = <- p",
            st,
        )
    )
    story.append(P("13.3 Larger project", st["h2"]))
    story.append(
        P(
            "The reference tree includes <font face='Courier'>sample/chatrelay/</font>, "
            "a multi-file concurrent TCP chat relay (roster with locks, async file "
            "logger over a pipe, per-client threads, line protocol). Parse it with:",
            st["body"],
        )
    )
    story.append(code_block("./munx --files sample/chatrelay/*.mx", st))

    # ----- 14 -----
    story.append(P("14. Implementation notes", st["h1"]))
    story.append(
        bullets(
            [
                "Build the front-end with <font face='Courier'>clang++ -std=c++20 -Iinclude src/main.cpp -o munx</font>.",
                "CLI: <font face='Courier'>./munx file.mx</font>, <font face='Courier'>--tokens</font>, <font face='Courier'>--files f0 f1 …</font>.",
                "Happy-path and error-path corpora live under <font face='Courier'>sample/valid/</font> and <font face='Courier'>sample/invalid/</font>.",
                "Expression nodes carry no type information — infer during analysis or codegen.",
                "<font face='Courier'>join [a,b]</font> lowers to a call; I/O and threading builtins are ordinary calls recognised by name.",
                "Suggested opcodes: <font face='Courier'>PUSH POP ADD CALL OPEN READ …</font> in <font face='Courier'>Opcode.hpp</font>.",
            ],
            st,
        )
    )
    story.append(Spacer(1, 8 * mm))
    story.append(
        P(
            "— End of specification —",
            st["caption"],
        )
    )

    doc.build(story, onFirstPage=footer, onLaterPages=footer)
    print(f"Wrote {OUT}")


if __name__ == "__main__":
    build()
