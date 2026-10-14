# Derive the pinned MIT font implementation; never edit the upstream checkout.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vendor/PDF4QT/Pdf4QtLibCore/sources/pdffont.cpp" font_source)
string(REPLACE "\r\n" "\n" font_source "${font_source}")
string(SHA256 font_hash "${font_source}")
if(NOT font_hash STREQUAL "85b9b3faaff6eb88e80f1cf267d9dcf9889affc7ebd42edb708d53eefadb331c")
    message(FATAL_ERROR "Review vertical font advances for the new PDF4QT source")
endif()
set(original_sequence "textSequence.items.emplace_back(&glyph.glyph, character, glyph.advance, cid);")
set(original_return "return PDFFontPointer(new PDFType0Font(qMove(cidSystemInfo), qMove(fontId), qMove(fontDescriptor), qMove(cmap), qMove(toUnicodeCMap), qMove(cidToGidMapper), defaultWidth, qMove(advances)));")
foreach(expected IN ITEMS "${original_sequence}" "${original_return}")
    string(FIND "${font_source}" "${expected}" found)
    if(found LESS 0)
        message(FATAL_ERROR "Pinned vertical font adaptation point is missing")
    endif()
endforeach()
# W2 is the declared vertical advance. FreeType's horizontal loading can yield
# zero Y advance; its synthesized metrics also omit PDF-specific tracking.
string(REPLACE "${original_sequence}"
    "const auto vertical = dynamic_cast<const tatsu::VerticalCidFont*>(font);\n                    const QPainterPath* path = vertical ? &getVerticalGlyph(*glyphIndex, vertical->origin(cid)) : &glyph.glyph;\n                    textSequence.items.emplace_back(path, character, m_isVertical ? glyphWidth * FONT_WIDTH_MULTIPLIER * m_pixelSize : glyph.advance, cid, mappedCode.byteCount == 1 && mappedCode.code == 32);"
    font_source "${font_source}")
set(original_missing_type0 "textSequence.items.emplace_back(nullpath, QChar(), -glyphWidth, cid);")
string(FIND "${font_source}" "${original_missing_type0}" missing_type0_index)
if(missing_type0_index LESS 0)
    message(FATAL_ERROR "Pinned missing Type0 glyph adaptation point is missing")
endif()
string(REPLACE "${original_missing_type0}"
    "textSequence.items.emplace_back(nullpath, QChar(), -glyphWidth, cid, mappedCode.byteCount == 1 && mappedCode.code == 32);"
    font_source "${font_source}")
set(vertical_widths [=[
            const auto horizontalAdvances = advances;
            std::unordered_map<CID, QPointF> verticalOrigins;
            if (cmap.isVertical())
            {
                defaultWidth = descendantFontDictionary->hasKey("DW2") ? dw2.back() : -1000.0;
                advances.clear();
                const auto metric = [&](const PDFObject& object)
                {
                    const auto& resolved = document->getObject(object);
                    if (!resolved.isInt() && !resolved.isReal())
                        throw PDFException(PDFTranslationContext::tr("Invalid vertical font metric."));
                    const auto value = fontLoader.readNumber(object, 0);
                    if (!std::isfinite(value))
                        throw PDFException(PDFTranslationContext::tr("Invalid vertical font metric."));
                    return value;
                };
                const PDFObject& verticalWidths = document->getObject(descendantFontDictionary->get("W2"));
                if (!verticalWidths.isNull() && !verticalWidths.isArray())
                    throw PDFException(PDFTranslationContext::tr("Invalid vertical font widths."));
                if (verticalWidths.isArray())
                {
                    const PDFArray* values = verticalWidths.getArray();
                    const size_t count = values->getCount();
                    for (size_t i = 0; i < count;)
                    {
                        const PDFInteger first = fontLoader.readInteger(values->getItem(i++), -1);
                        if (first < 0 || first > 65535 || i >= count)
                            throw PDFException(PDFTranslationContext::tr("Invalid vertical font CID."));
                        const PDFObject& next = document->getObject(values->getItem(i++));
                        if (next.isArray())
                        {
                            const PDFArray* triples = next.getArray();
                            const size_t size = triples->getCount();
                            if (!size || size % 3 || size / 3 > size_t(65536 - first))
                                throw PDFException(PDFTranslationContext::tr("Invalid vertical font width triples."));
                            for (size_t j = 0; j < size; j += 3)
                            {
                                const CID cid = CID(first + PDFInteger(j / 3));
                                advances[cid] = metric(triples->getItem(j));
                                verticalOrigins[cid] = {metric(triples->getItem(j + 1)), metric(triples->getItem(j + 2))};
                            }
                        }
                        else
                        {
                            const PDFInteger last = fontLoader.readInteger(next, -1);
                            if (last < first || last > 65535 || count - i < 3)
                                throw PDFException(PDFTranslationContext::tr("Invalid vertical font width range."));
                            const PDFReal width = metric(values->getItem(i));
                            const QPointF origin(metric(values->getItem(i + 1)), metric(values->getItem(i + 2)));
                            i += 3;
                            for (PDFInteger cid = first; cid <= last; ++cid)
                            {
                                advances[CID(cid)] = width;
                                verticalOrigins[CID(cid)] = origin;
                            }
                        }
                    }
                }
                auto result = new tatsu::VerticalCidFont(qMove(cidSystemInfo), qMove(fontId), qMove(fontDescriptor), qMove(cmap), qMove(toUnicodeCMap), qMove(cidToGidMapper), defaultWidth, qMove(advances));
                result->configureOrigins(descendantFontDictionary->hasKey("DW2") ? dw2.front() : 880.0, dw, horizontalAdvances, qMove(verticalOrigins));
                return PDFFontPointer(result);
            }
]=])
string(REPLACE "${original_return}" "${vertical_widths}            ${original_return}" font_source "${font_source}")
set(original_glyph_declaration "const Glyph& getGlyph(unsigned int glyphIndex);")
set(original_glyph_cache "std::unordered_map<unsigned int, Glyph> m_glyphCache;")
set(original_glyph_definition "const PDFRealizedFontImpl::Glyph& PDFRealizedFontImpl::getGlyph(unsigned int glyphIndex)")
foreach(expected IN ITEMS "${original_glyph_declaration}" "${original_glyph_cache}" "${original_glyph_definition}")
    string(FIND "${font_source}" "${expected}" found)
    if(found LESS 0)
        message(FATAL_ERROR "Pinned vertical glyph adaptation point is missing")
    endif()
endforeach()
string(REPLACE "#include \"pdffont.h\"" "#include \"pdffont.h\"\n#include \"vertical_cid_font.h\"\n#include <cmath>\n#include <tuple>" font_source "${font_source}")
string(REPLACE "${original_glyph_declaration}" "${original_glyph_declaration}\n    const QPainterPath& getVerticalGlyph(unsigned int glyphIndex, QPointF origin);" font_source "${font_source}")
string(REPLACE "${original_glyph_cache}" "${original_glyph_cache}\n    std::map<std::tuple<unsigned int, PDFReal, PDFReal>, QPainterPath> m_verticalGlyphCache;" font_source "${font_source}")
set(vertical_glyph [=[
const QPainterPath& PDFRealizedFontImpl::getVerticalGlyph(unsigned int glyphIndex, QPointF origin)
{
    const auto key = std::make_tuple(glyphIndex, origin.x(), origin.y());
    {
        QReadLocker lock(&m_readWriteLock);
        if (const auto found = m_verticalGlyphCache.find(key); found != m_verticalGlyphCache.end())
            return found->second;
    }
    const auto path = getGlyph(glyphIndex).glyph;
    QWriteLocker lock(&m_readWriteLock);
    return m_verticalGlyphCache.emplace(key, path.translated(-origin.x() * FONT_WIDTH_MULTIPLIER * m_pixelSize,
                                                            -origin.y() * FONT_WIDTH_MULTIPLIER * m_pixelSize)).first->second;
}

]=])
string(REPLACE "${original_glyph_definition}" "${vertical_glyph}${original_glyph_definition}" font_source "${font_source}")
set(adapted_font "${CMAKE_BINARY_DIR}/adapted/pdffont.cpp")
if(EXISTS "${adapted_font}")
    file(READ "${adapted_font}" previous_font)
endif()
if(NOT font_source STREQUAL previous_font)
    file(WRITE "${adapted_font}" "${font_source}")
endif()
get_target_property(core_sources Pdf4QtLibCore SOURCES)
list(FIND core_sources "sources/pdffont.cpp" original_index)
if(original_index LESS 0)
    message(FATAL_ERROR "Pinned font source is missing from target")
endif()
list(REMOVE_ITEM core_sources "sources/pdffont.cpp")
set_property(TARGET Pdf4QtLibCore PROPERTY SOURCES "${core_sources};${adapted_font}")
target_include_directories(Pdf4QtLibCore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/PDF4QT/Pdf4QtLibCore/sources")

# Preserve Japanese column tracking without inventing word separators.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vendor/PDF4QT/Pdf4QtLibCore/sources/pdftextlayout.cpp" layout_source)
string(REPLACE "\r\n" "\n" layout_source "${layout_source}")
string(SHA256 layout_hash "${layout_source}")
if(NOT layout_hash STREQUAL "6def0c7ead25df81418ee8411bf9f6b411d603d68e9b22c8d85132fb1d4dd3fd")
    message(FATAL_ERROR "Review Japanese vertical text spacing for the new PDF4QT source")
endif()
set(original_spacing "if (!previousCharacter.character.isSpace() && QLineF(previousCharacter.position, currentCharacter.position).length() > previousCharacter.advance * 1.2)")
string(FIND "${layout_source}" "${original_spacing}" original_spacing_index)
if(original_spacing_index LESS 0)
    message(FATAL_ERROR "Pinned text spacing adaptation point is missing")
endif()
string(REPLACE "${original_spacing}"
    "if (!previousCharacter.character.isSpace() && !tatsu::keepVerticalJapaneseSpacing(textLine.getAngle(), previousCharacter.character, currentCharacter.character) && QLineF(previousCharacter.position, currentCharacter.position).length() > previousCharacter.advance * 1.2)"
    layout_source "${layout_source}")
string(REPLACE "#include \"pdftextlayout.h\"" "#include \"pdftextlayout.h\"\n#include \"vertical_text_spacing.h\""
    layout_source "${layout_source}")
set(adapted_layout "${CMAKE_BINARY_DIR}/adapted/pdftextlayout.cpp")
if(EXISTS "${adapted_layout}")
    file(READ "${adapted_layout}" previous_layout)
endif()
if(NOT layout_source STREQUAL previous_layout)
    file(WRITE "${adapted_layout}" "${layout_source}")
endif()
get_target_property(core_sources Pdf4QtLibCore SOURCES)
list(FIND core_sources "sources/pdftextlayout.cpp" original_layout_index)
if(original_layout_index LESS 0)
    message(FATAL_ERROR "Pinned text layout source missing from target")
endif()
list(REMOVE_ITEM core_sources "sources/pdftextlayout.cpp")
set_property(TARGET Pdf4QtLibCore PROPERTY SOURCES "${core_sources};${adapted_layout}")
target_include_directories(Pdf4QtLibCore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
