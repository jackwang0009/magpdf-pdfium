// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/edit/cpdf_pagecontentgenerator.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

#include "constants/page_object.h"
#include "core/fpdfapi/edit/cpdf_pagecontentmanager.h"
#include "core/fpdfapi/edit/cpdf_stringarchivestream.h"
#include "core/fpdfapi/font/cpdf_truetypefont.h"
#include "core/fpdfapi/font/cpdf_type1font.h"
#include "core/fpdfapi/page/cpdf_clippath.h"
#include "core/fpdfapi/page/cpdf_color.h"
#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/page/cpdf_contentmarks.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_path.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_pattern.h"
#include "core/fpdfapi/page/cpdf_shadingobject.h"
#include "core/fpdfapi/page/cpdf_shadingpattern.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/page/cpdf_textstate.h"
#include "core/fpdfapi/page/cpdf_tilingpattern.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fpdfapi/parser/object_tree_traversal_util.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/containers/contains.h"
#include "core/fxcrt/notreached.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/stream_write_utils.h"
#include "core/fxcrt/zip.h"

namespace {

// TODO(thestig): Remove/restore other unused resource types:
// - ColorSpace
// - Pattern
// - Shading
constexpr const char* kResourceKeys[] = {"ExtGState", "Font", "XObject"};

// Key: The resource type.
// Value: The resource names of a given type.
using ResourcesMap = std::map<ByteString, std::set<ByteString>>;

// The initial values of the graphics state parameters that are only written
// when they have another value.
constexpr float kDefaultMiterLimit = 10.0f;
constexpr float kDefaultFlatness = 1.0f;

// Balances the "q" operator ProcessGraphics() emitted.
void EndProcessGraphics(fxcrt::ostringstream& buf) {
  buf << " Q\n";
}

// A soft mask is laid out in the coordinate system that was current when the
// graphics state parameter dictionary that sets it was applied, and the
// object remembers that matrix. Objects are written with the identity matrix
// as the current one, so the remembered matrix has to be made current for
// that dictionary, with "cm" operators around its "gs" operator.
//
// Returns the index of that dictionary among the ones recorded for
// `page_object`, if there is one and the matrix is not the identity matrix.
// A matrix that cannot be inverted is of no use: the current matrix could
// not be restored after it.
std::optional<size_t> FindSoftMaskDictThatNeedsMatrix(
    const CPDF_PageObject* page_object) {
  const CPDF_GeneralState& general_state = page_object->general_state();
  RetainPtr<const CPDF_Dictionary> soft_mask = general_state.GetSoftMask();
  if (!soft_mask) {
    return std::nullopt;
  }
  const CFX_Matrix* matrix = general_state.GetSMaskMatrix();
  if (!matrix || matrix->IsIdentity() ||
      matrix->a * matrix->d == matrix->b * matrix->c) {
    return std::nullopt;
  }
  pdfium::span<const RetainPtr<CPDF_Dictionary>> dicts =
      general_state.GetGraphicsResourceDicts();
  for (size_t i = dicts.size(); i-- > 0;) {
    if (dicts[i]->GetDirectObjectFor("SMask").Get() == soft_mask.Get()) {
      return i;
    }
  }
  return std::nullopt;
}

void RecordPageObjectResourceUsage(const CPDF_PageObject* page_object,
                                   ResourcesMap& seen_resources) {
  const ByteString& resource_name = page_object->GetResourceName();
  if (!resource_name.IsEmpty()) {
    switch (page_object->GetType()) {
      case CPDF_PageObject::Type::kText:
        seen_resources["Font"].insert(resource_name);
        break;
      case CPDF_PageObject::Type::kImage:
      case CPDF_PageObject::Type::kForm:
        seen_resources["XObject"].insert(resource_name);
        break;
      case CPDF_PageObject::Type::kPath:
        break;
      case CPDF_PageObject::Type::kShading:
        break;
    }
  }
  for (const auto& name : page_object->GetGraphicsResourceNames()) {
    CHECK(!name.IsEmpty());
    seen_resources["ExtGState"].insert(name);
  }
}

CPDF_PageObjectHolder::RemovedResourceMap RemoveUnusedResources(
    CPDF_Dictionary* resource_dict,
    const std::vector<ByteString>& keys,
    const std::set<ByteString>* resource_in_use_of_current_type) {
  CPDF_PageObjectHolder::RemovedResourceMap removed_resource_map;
  for (const ByteString& key : keys) {
    if (resource_in_use_of_current_type &&
        pdfium::Contains(*resource_in_use_of_current_type, key)) {
      continue;
    }

    removed_resource_map[key] = resource_dict->RemoveFor(key.AsStringView());
  }
  return removed_resource_map;
}

void SaveUnusedResources(
    CPDF_PageObjectHolder::RemovedResourceMap& removed_resource_map,
    CPDF_PageObjectHolder::RemovedResourceMap& saved_resource_map) {
  for (auto& entry : removed_resource_map) {
    saved_resource_map[entry.first] = std::move(entry.second);
  }
}

std::vector<ByteString> FindKeysToRestore(
    const std::vector<ByteString>& keys,
    const std::set<ByteString>* resource_in_use_of_current_type,
    const CPDF_PageObjectHolder::RemovedResourceMap& saved_resource_map) {
  if (!resource_in_use_of_current_type) {
    return {};
  }

  std::vector<ByteString> missing_keys;
  for (const ByteString& key : *resource_in_use_of_current_type) {
    if (!pdfium::Contains(keys, key)) {
      missing_keys.push_back(key);
    }
  }

  std::vector<ByteString> keys_to_restore;
  for (const ByteString& key : missing_keys) {
    if (pdfium::Contains(saved_resource_map, key)) {
      keys_to_restore.push_back(key);
    }
  }
  return keys_to_restore;
}

// `keys_to_restore` entries must exist in `saved_resource_map`.
void RestoreUsedResources(
    CPDF_Dictionary* resource_dict,
    const std::vector<ByteString>& keys_to_restore,
    CPDF_PageObjectHolder::RemovedResourceMap& saved_resource_map) {
  for (const ByteString& key : keys_to_restore) {
    auto node = saved_resource_map.extract(key);
    CHECK(!node.empty());
    resource_dict->SetFor(node.key(), std::move(node.mapped()));
  }
}

void RemoveOrRestoreUnusedResources(
    RetainPtr<CPDF_Dictionary> resources_dict,
    const ResourcesMap& resources_in_use,
    CPDF_PageObjectHolder::AllRemovedResourcesMap& all_removed_resources_map) {
  for (const char* resource_key : kResourceKeys) {
    auto resources_in_use_it = resources_in_use.find(resource_key);
    const std::set<ByteString>* resource_in_use_of_current_type =
        resources_in_use_it != resources_in_use.end()
            ? &resources_in_use_it->second
            : nullptr;

    RetainPtr<CPDF_Dictionary> current_resource_dict =
        resources_dict->GetMutableDictFor(resource_key);
    if (!current_resource_dict && !resource_in_use_of_current_type) {
      continue;
    }

    const std::vector<ByteString> keys = current_resource_dict->GetKeys();
    CPDF_PageObjectHolder::RemovedResourceMap removed_resource_map =
        RemoveUnusedResources(current_resource_dict, keys,
                              resource_in_use_of_current_type);
    if (!removed_resource_map.empty()) {
      // Note that this may create a new entry in `all_removed_resources_map`.
      // Only do this if `removed_resource_map` is not empty.
      SaveUnusedResources(removed_resource_map,
                          all_removed_resources_map[resource_key]);
    }

    // Restore in-use objects if possible. To do so, first see if there is a
    // saved resource map to restore from.
    auto saved_resource_map_it = all_removed_resources_map.find(resource_key);
    if (saved_resource_map_it == all_removed_resources_map.end()) {
      continue;
    }

    auto& saved_resource_map = saved_resource_map_it->second;
    std::vector<ByteString> keys_to_restore = FindKeysToRestore(
        keys, resource_in_use_of_current_type, saved_resource_map);
    if (keys_to_restore.empty()) {
      continue;
    }

    // Create the dictionary if needed, so there is a place to restore to.
    if (!current_resource_dict) {
      current_resource_dict =
          resources_dict->SetNewFor<CPDF_Dictionary>(resource_key);
    }
    RestoreUsedResources(current_resource_dict, keys_to_restore,
                         saved_resource_map);
  }
}

bool IsPageResourceShared(CPDF_Document* doc,
                          RetainPtr<const CPDF_Dictionary> page_dict,
                          RetainPtr<const CPDF_Dictionary> resources_dict) {
  CHECK(doc);
  CHECK(page_dict);
  CHECK(resources_dict);

  const uint32_t resources_object_number = resources_dict->GetObjNum();
  if (resources_object_number) {
    // If `resources_dict` is not an inline object, then check to see if is a
    // shared object.
    if (pdfium::Contains(GetObjectsWithMultipleReferences(doc),
                         resources_object_number)) {
      return true;
    }
  }

  // The check above may not catch all cases. e.g. inline objects. Check all
  // pages in the document to see if another page is using the same resources.
  int page_dict_seen = 0;
  int resources_dict_seen = 0;
  for (int i = 0; i < doc->GetPageCount(); ++i) {
    RetainPtr<CPDF_Dictionary> current_page_dict =
        doc->GetMutablePageDictionary(i);
    if (!current_page_dict) {
      continue;
    }

    // Check to see if the current page's page dictionary is seen twice: Once
    // for this page, and once for another. If the same page dictionary is in
    // use twice, then the resource dictionary within must also be shared.
    if (current_page_dict == page_dict) {
      ++page_dict_seen;
      if (page_dict_seen == 2) {
        return true;
      }
    }

    // Similar check as above, for the current page's resource dictionary.
    auto page =
        pdfium::MakeRetain<CPDF_Page>(doc, std::move(current_page_dict));
    if (resources_dict == page->GetMutableResources()) {
      ++resources_dict_seen;
      if (resources_dict_seen == 2) {
        return true;
      }
    }
  }

  return false;
}

void CloneResourcesDictEntries(CPDF_Document* doc,
                               RetainPtr<CPDF_Dictionary> resources_dict) {
  struct KeyAndObject {
    ByteString key;
    RetainPtr<const CPDF_Object> object;
  };
  std::vector<KeyAndObject> entries_to_maybe_clone;
  {
    CPDF_DictionaryLocker locker(resources_dict);
    for (const auto& it : locker) {
      const ByteString& key = it.first;
      const RetainPtr<CPDF_Object>& object = it.second;
      // Clone resource dictionaries that may get modified. While out for
      // self-referencing loops.
      if (object != resources_dict &&
          object->GetType() == CPDF_Object::kReference &&
          pdfium::Contains(kResourceKeys, key)) {
        RetainPtr<const CPDF_Object> direct_object = object->GetDirect();
        if (direct_object) {
          entries_to_maybe_clone.emplace_back(key, std::move(direct_object));
        }
      }
    }
  }

  if (entries_to_maybe_clone.empty()) {
    return;
  }

  std::set<uint32_t> shared_objects = GetObjectsWithMultipleReferences(doc);
  if (shared_objects.empty()) {
    return;
  }

  // Must modify `resources_dict` after `locker` goes out of scope.
  for (const auto& entry : entries_to_maybe_clone) {
    if (pdfium::Contains(shared_objects, entry.object->GetObjNum())) {
      const uint32_t clone_object_number =
          doc->AddIndirectObject(entry.object->Clone());
      resources_dict->SetNewFor<CPDF_Reference>(entry.key, doc,
                                                clone_object_number);
    }
  }
}

// The abbreviations an inline image uses for the keys of its dictionary and
// for the names of color spaces and filters. See ISO 32000-1:2008, tables 93
// and 94. The parser expands them all, so they are abbreviated again when
// the inline image is written back.
struct InlineImageAbbreviation {
  const char* full_name;
  const char* abbreviation;
};

constexpr InlineImageAbbreviation kInlineImageKeyAbbreviations[] = {
    {"BitsPerComponent", "BPC"}, {"ColorSpace", "CS"}, {"Decode", "D"},
    {"DecodeParms", "DP"},       {"Filter", "F"},      {"Height", "H"},
    {"ImageMask", "IM"},         {"Interpolate", "I"}, {"Width", "W"},
};

constexpr InlineImageAbbreviation kInlineImageColorSpaceAbbreviations[] = {
    {"DeviceGray", "G"},
    {"DeviceRGB", "RGB"},
    {"DeviceCMYK", "CMYK"},
    {"Indexed", "I"},
};

constexpr InlineImageAbbreviation kInlineImageFilterAbbreviations[] = {
    {"ASCIIHexDecode", "AHx"}, {"ASCII85Decode", "A85"},
    {"LZWDecode", "LZW"},      {"FlateDecode", "Fl"},
    {"RunLengthDecode", "RL"}, {"CCITTFaxDecode", "CCF"},
    {"DCTDecode", "DCT"},
};

ByteString AbbreviateInlineImageName(
    pdfium::span<const InlineImageAbbreviation> abbreviations,
    const ByteString& name) {
  for (const auto& entry : abbreviations) {
    if (name == entry.full_name) {
      return ByteString(entry.abbreviation);
    }
  }
  return name;
}

bool IsDeviceColorSpaceName(const ByteString& name) {
  return name == "DeviceGray" || name == "DeviceRGB" || name == "DeviceCMYK";
}

// Whether `object` is a color space an inline image can spell out in its
// dictionary: a device color space, or an Indexed color space based on one
// with its lookup table in a string. Every other color space has to be the
// name of a color space resource.
bool IsInlineImageColorSpace(const CPDF_Object* object) {
  if (const CPDF_Name* name = object->AsName()) {
    return IsDeviceColorSpaceName(name->GetString());
  }
  const CPDF_Array* array = object->AsArray();
  if (!array) {
    return false;
  }
  if (array->size() == 1) {
    RetainPtr<const CPDF_Object> family = array->GetObjectAt(0);
    return family->IsName() && IsDeviceColorSpaceName(family->GetString());
  }
  if (array->size() != 4) {
    return false;
  }
  // GetObjectAt() does not resolve references, which cannot be written to a
  // content stream.
  RetainPtr<const CPDF_Object> family = array->GetObjectAt(0);
  RetainPtr<const CPDF_Object> base = array->GetObjectAt(1);
  RetainPtr<const CPDF_Object> hival = array->GetObjectAt(2);
  RetainPtr<const CPDF_Object> lookup = array->GetObjectAt(3);
  return family->IsName() && family->GetString() == "Indexed" &&
         base->IsName() && IsDeviceColorSpaceName(base->GetString()) &&
         hival->IsNumber() && lookup->IsString();
}

// Writes `object` as an operand in the dictionary of an inline image, with
// the names in `abbreviations` abbreviated. Returns false for an object that
// cannot be written to a content stream: an indirect reference, a stream, or
// something nested too deeply to be a sane operand.
bool WriteInlineImageOperand(
    fxcrt::ostringstream& buf,
    const CPDF_Object* object,
    pdfium::span<const InlineImageAbbreviation> abbreviations,
    int depth) {
  static constexpr int kMaxDepth = 8;
  if (depth > kMaxDepth) {
    return false;
  }
  switch (object->GetType()) {
    case CPDF_Object::kBoolean:
    case CPDF_Object::kNumber:
      buf << object->GetString();
      return true;
    case CPDF_Object::kString:
      buf << object->AsString()->EncodeString();
      return true;
    case CPDF_Object::kName:
      buf << "/"
          << PDF_NameEncode(
                 AbbreviateInlineImageName(abbreviations, object->GetString()));
      return true;
    case CPDF_Object::kNullobj:
      buf << "null";
      return true;
    case CPDF_Object::kArray: {
      const CPDF_Array* array = object->AsArray();
      buf << "[";
      for (size_t i = 0; i < array->size(); ++i) {
        if (i > 0) {
          buf << " ";
        }
        if (!WriteInlineImageOperand(buf, array->GetObjectAt(i).Get(),
                                     abbreviations, depth + 1)) {
          return false;
        }
      }
      buf << "]";
      return true;
    }
    case CPDF_Object::kDictionary: {
      // The keys of a nested dictionary (/DecodeParms) are never abbreviated.
      CPDF_DictionaryLocker locker(object->AsDictionary());
      buf << "<<";
      for (const auto& it : locker) {
        buf << "/" << PDF_NameEncode(it.first) << " ";
        if (!WriteInlineImageOperand(buf, it.second.Get(), abbreviations,
                                     depth + 1)) {
          return false;
        }
        buf << " ";
      }
      buf << ">>";
      return true;
    }
    case CPDF_Object::kStream:
    case CPDF_Object::kReference:
      return false;
  }
  return false;
}

// A text object reduced to everything that determines the glyph outlines
// it contributes to a clip path. Two texts with equal signatures contribute
// identical outlines, so a text found in a later object's clip path can be
// matched against the page objects by value: the clip path holds clones of
// the text objects, not the page objects themselves.
struct TextClipSignature {
  RetainPtr<CPDF_Font> font;
  float font_size = 0;
  CFX_Matrix text_matrix;
  CFX_PointF pos;
  CFX_Matrix matrix;
  std::array<float, 4> text_ctm = {1, 0, 0, 1};
  int render_mode = 0;
  std::vector<uint32_t> char_codes;
  std::vector<float> char_kernings;
  std::vector<float> char_positions;
  float line_width = 1.0f;
  int line_cap = 0;
  int line_join = 0;
  std::vector<float> dash_array;
  float dash_phase = 0;

  bool operator==(const TextClipSignature&) const = default;
};

TextClipSignature GetTextClipSignature(const CPDF_TextObject* text) {
  TextClipSignature signature;
  signature.font = text->GetFont();
  signature.font_size = text->GetFontSize();
  signature.text_matrix = text->GetTextMatrix();
  signature.pos = text->GetPos();
  // Text objects have no separate path/image matrix(); Transform() folds
  // into the text matrix / position already captured above.
  signature.matrix = CFX_Matrix();
  pdfium::span<const float, 4> ctm = text->text_state().GetCTM();
  for (size_t i = 0; i < 4; ++i) {
    signature.text_ctm[i] = ctm[i];
  }
  signature.render_mode = static_cast<int>(text->GetTextRenderMode());
  signature.char_codes = text->GetCharCodes();
  signature.char_kernings = text->GetCharKernings();
  signature.char_positions = text->GetCharPositions();
  const CFX_GraphState& graph = text->graph_state();
  signature.line_width = graph.GetLineWidth();
  signature.line_cap = static_cast<int>(graph.GetLineCap());
  signature.line_join = static_cast<int>(graph.GetLineJoin());
  signature.dash_array = graph.GetLineDashArray();
  signature.dash_phase = graph.GetLineDashPhase();
  return signature;
}

// Splits the text list of `clip_path` into its null-terminated groups, one
// per BT...ET that established the clip. The renderer unions the glyphs
// within a group and intersects the groups.
std::vector<std::vector<TextClipSignature>> GetClipTextGroups(
    const CPDF_ClipPath& clip_path) {
  std::vector<std::vector<TextClipSignature>> groups;
  if (!clip_path.HasRef()) {
    return groups;
  }
  std::vector<TextClipSignature> current;
  for (size_t i = 0; i < clip_path.GetTextCount(); ++i) {
    CPDF_TextObject* text = clip_path.GetText(i);
    if (!text) {
      if (!current.empty()) {
        groups.push_back(std::move(current));
        current.clear();
      }
    } else {
      current.push_back(GetTextClipSignature(text));
    }
  }
  if (!current.empty()) {
    groups.push_back(std::move(current));
  }
  return groups;
}

// Whether two clip paths paint the same path clip: the same paths with the
// same fill types, in the same order. The text list is not compared: it is
// the clip the text itself establishes, not the clip that clips the text.
bool ClipPathsEqual(const CPDF_ClipPath& lhs, const CPDF_ClipPath& rhs) {
  const size_t lhs_count = lhs.HasRef() ? lhs.GetPathCount() : 0;
  const size_t rhs_count = rhs.HasRef() ? rhs.GetPathCount() : 0;
  if (lhs_count != rhs_count) {
    return false;
  }
  for (size_t i = 0; i < lhs_count; ++i) {
    if (lhs.GetClipType(i) != rhs.GetClipType(i)) {
      return false;
    }
    CPDF_Path lhs_path = lhs.GetPath(i);
    CPDF_Path rhs_path = rhs.GetPath(i);
    pdfium::span<const CFX_Path::Point> lhs_points = lhs_path.GetPoints();
    pdfium::span<const CFX_Path::Point> rhs_points = rhs_path.GetPoints();
    if (lhs_points.size() != rhs_points.size()) {
      return false;
    }
    for (size_t j = 0; j < lhs_points.size(); ++j) {
      if (lhs_points[j].point_ != rhs_points[j].point_ ||
          lhs_points[j].type_ != rhs_points[j].type_ ||
          lhs_points[j].close_figure_ != rhs_points[j].close_figure_) {
        return false;
      }
    }
  }
  return true;
}

// Whether the groups of `needle` appear in `haystack` as a contiguous
// subsequence, in order. Clip groups nest: a group established inside
// another group is only in effect while the outer group is, so the groups
// a clip holds always appear in order.
bool ClipGroupsContain(
    const std::vector<std::vector<TextClipSignature>>& haystack,
    const std::vector<std::vector<TextClipSignature>>& needle) {
  if (needle.empty() || needle.size() > haystack.size()) {
    return false;
  }
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      if (haystack[i + j] != needle[j]) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

// The clip groups of a later object that the tail of `run` provides, as
// the groups themselves (in order). Empty when the run provides none of
// the later object's clip. The longest match wins: a run that spans two
// BT...ETs matches both of the later object's groups, and a run whose
// first BT is already closed matches only the second group's texts.
std::vector<std::vector<TextClipSignature>> MatchClipRun(
    pdfium::span<const TextClipSignature> run,
    const std::vector<std::vector<TextClipSignature>>& clip_groups) {
  std::vector<TextClipSignature> flat;
  std::vector<size_t> starts;
  for (const auto& group : clip_groups) {
    starts.push_back(flat.size());
    flat.insert(flat.end(), group.begin(), group.end());
  }
  for (size_t s = 0; s < starts.size(); ++s) {
    const size_t len = flat.size() - starts[s];
    if (len == 0 || len > run.size()) {
      continue;
    }
    bool equal = true;
    for (size_t i = 0; i < len; ++i) {
      if (!(flat[starts[s] + i] == run[run.size() - len + i])) {
        equal = false;
        break;
      }
    }
    if (!equal) {
      continue;
    }
    return std::vector<std::vector<TextClipSignature>>(
        clip_groups.begin() + s, clip_groups.end());
  }
  return {};
}

}  // namespace

CPDF_PageContentGenerator::CPDF_PageContentGenerator(
    CPDF_PageObjectHolder* pObjHolder)
    : obj_holder_(pObjHolder), document_(pObjHolder->GetDocument()) {
  // Copy all page objects, even if they are inactive. They are needed in
  // GenerateModifiedStreams() below.
  for (const auto& pObj : *pObjHolder) {
    page_objects_.emplace_back(pObj.get());
  }
}

CPDF_PageContentGenerator::~CPDF_PageContentGenerator() = default;

void CPDF_PageContentGenerator::GenerateContent() {
  std::map<int32_t, fxcrt::ostringstream> new_stream_data =
      GenerateModifiedStreams();
  // If no streams were regenerated or removed, nothing to do here.
  if (new_stream_data.empty()) {
    return;
  }

  UpdateContentStreams(std::move(new_stream_data));
  UpdateResourcesDict();
}

std::map<int32_t, fxcrt::ostringstream>
CPDF_PageContentGenerator::GenerateModifiedStreams() {
  // Figure out which streams are dirty.
  std::set<int32_t> all_dirty_streams;
  for (auto& pPageObj : page_objects_) {
    // Must include dirty page objects even if they are marked as inactive.
    // Otherwise an inactive object will not be detected that its stream needs
    // to be removed as part of regeneration.
    if (pPageObj->IsDirty()) {
      all_dirty_streams.insert(pPageObj->GetContentStream());
    }
  }
  std::set<int32_t> marked_dirty_streams = obj_holder_->TakeDirtyStreams();
  all_dirty_streams.insert(marked_dirty_streams.begin(),
                           marked_dirty_streams.end());

  // Start regenerating dirty streams.
  std::map<int32_t, fxcrt::ostringstream> streams;
  std::set<int32_t> empty_streams;
  std::unique_ptr<const CPDF_ContentMarks> empty_content_marks =
      std::make_unique<CPDF_ContentMarks>();
  std::map<int32_t, const CPDF_ContentMarks*> current_content_marks;

  for (int32_t dirty_stream : all_dirty_streams) {
    fxcrt::ostringstream buf;

    // Set the default graphic state values. Update CTM to be the identity
    // matrix for the duration of this stream, if it is not already.
    buf << "q\n";
    const CFX_Matrix ctm = obj_holder_->GetCTMAtBeginningOfStream(dirty_stream);
    if (!ctm.IsIdentity()) {
      WriteMatrix(buf, ctm.GetInverse()) << " cm\n";
    }

    ProcessDefaultGraphics(&buf);
    streams[dirty_stream] = std::move(buf);
    empty_streams.insert(dirty_stream);
    current_content_marks[dirty_stream] = empty_content_marks.get();
  }

  // Process the page objects, write into each dirty stream.
  //
  // A text clip group (Tr 4-7) keeps its `q` open across the objects it
  // clips: the clip a BT...ET establishes only survives while the `q`
  // around it stays open, and the objects the text clips are exactly the
  // ones whose clip path still holds the group's texts. Writing the text
  // on its own (as ProcessText() does) closes the `q` right away, so the
  // clipped objects would paint unclipped.
  struct OpenClipGroup {
    // The BT...ET groups of the clip, in order.
    std::vector<std::vector<TextClipSignature>> groups;
  };
  std::vector<OpenClipGroup> open_groups;
  int32_t current_stream = CPDF_PageObject::kNoContentStream;
  auto close_groups_not_in = [&](fxcrt::ostringstream* buf,
                                 const CPDF_PageObject* obj) {
    std::vector<std::vector<TextClipSignature>> groups =
        GetClipTextGroups(obj->clip_path());
    while (!open_groups.empty() &&
           !ClipGroupsContain(groups, open_groups.back().groups)) {
      *buf << " Q\n";
      open_groups.pop_back();
    }
  };
  auto close_all_groups = [&](int32_t stream_index) {
    auto stream_it = streams.find(stream_index);
    if (stream_it == streams.end()) {
      return;
    }
    while (!open_groups.empty()) {
      stream_it->second << " Q\n";
      open_groups.pop_back();
    }
  };

  for (size_t obj_index = 0; obj_index < page_objects_.size(); ++obj_index) {
    CPDF_PageObject* pPageObj = page_objects_[obj_index].get();
    if (!pPageObj->IsActive()) {
      continue;
    }

    int stream_index = pPageObj->GetContentStream();
    auto it = streams.find(stream_index);
    if (it == streams.end()) {
      continue;
    }

    if (stream_index != current_stream) {
      // A clip cannot span content streams: the `q` that holds it must
      // close in the stream that opened it.
      if (current_stream != CPDF_PageObject::kNoContentStream) {
        close_all_groups(current_stream);
      }
      current_stream = stream_index;
    }

    fxcrt::ostringstream* buf = &it->second;
    empty_streams.erase(stream_index);

    CPDF_TextObject* pTextObj = pPageObj->AsText();
    if (pTextObj &&
        TextRenderingModeIsClipMode(pTextObj->GetTextRenderMode())) {
      // Collect the run of consecutive clip-mode texts in this stream.
      std::vector<CPDF_TextObject*> run;
      std::vector<TextClipSignature> run_signatures;
      size_t run_end = obj_index;
      for (size_t j = obj_index; j < page_objects_.size(); ++j) {
        CPDF_PageObject* candidate = page_objects_[j].get();
        if (!candidate->IsActive() ||
            candidate->GetContentStream() != stream_index) {
          break;
        }
        CPDF_TextObject* candidate_text = candidate->AsText();
        if (!candidate_text ||
            !TextRenderingModeIsClipMode(
                candidate_text->GetTextRenderMode())) {
          break;
        }
        run.push_back(candidate_text);
        run_signatures.push_back(GetTextClipSignature(candidate_text));
        run_end = j + 1;
      }

      // The first active object after the run decides: if its clip holds
      // the run's texts, the run's `q` must stay open across it. Any
      // object after the run that does not hold them proves the `q` was
      // already closed, so a later object holding the same glyphs again
      // is a new clip, not this one.
      const CPDF_PageObject* clip_user = nullptr;
      for (size_t j = run_end; j < page_objects_.size(); ++j) {
        CPDF_PageObject* candidate = page_objects_[j].get();
        if (candidate->GetContentStream() != stream_index) {
          break;
        }
        if (!candidate->IsActive()) {
          continue;
        }
        clip_user = candidate;
        break;
      }

      // The matched groups, longest first: drop leading groups whose
      // texts cannot be grouped (unwritable font, content marks, or a
      // path clip that differs from the first text's) and retry.
      std::vector<std::vector<TextClipSignature>> matched;
      if (clip_user) {
        matched = MatchClipRun(run_signatures,
                               GetClipTextGroups(clip_user->clip_path()));
      }
      std::vector<CPDF_TextObject*> grouped_texts;
      std::vector<size_t> group_sizes;
      size_t used_drop = matched.size();
      for (size_t drop = 0; drop < matched.size(); ++drop) {
        size_t count = 0;
        for (size_t g = drop; g < matched.size(); ++g) {
          count += matched[g].size();
        }
        std::vector<CPDF_TextObject*> candidate_texts(run.end() - count,
                                                      run.end());
        bool usable = true;
        for (CPDF_TextObject* text : candidate_texts) {
          // The graphics state of a grouped text is written between BT and
          // ET, where "cm" is not allowed.
          if (!CanWriteText(text) ||
              FindSoftMaskDictThatNeedsMatrix(text).has_value()) {
            usable = false;
            break;
          }
          const CPDF_ContentMarks* marks = text->GetContentMarks();
          if (marks && marks->CountItems() != 0) {
            usable = false;
            break;
          }
        }
        if (usable) {
          for (size_t i = 1; i < candidate_texts.size(); ++i) {
            if (!ClipPathsEqual(candidate_texts[0]->clip_path(),
                               candidate_texts[i]->clip_path())) {
              usable = false;
              break;
            }
          }
        }
        if (usable) {
          grouped_texts = std::move(candidate_texts);
          for (size_t g = drop; g < matched.size(); ++g) {
            group_sizes.push_back(matched[g].size());
          }
          used_drop = drop;
          break;
        }
      }

      if (!grouped_texts.empty()) {
        // Write the texts before the grouped tail one by one (their clips
        // are closed already), then the tail as one clip group.
        const size_t head_count = run.size() - grouped_texts.size();
        for (size_t i = 0; i < head_count; ++i) {
          CPDF_PageObject* head = run[i];
          close_groups_not_in(buf, head);
          current_content_marks[stream_index] = ProcessContentMarks(
              buf, head, current_content_marks[stream_index]);
          ProcessPageObject(buf, head);
        }
        close_groups_not_in(buf, grouped_texts.front());
        current_content_marks[stream_index] = ProcessContentMarks(
            buf, grouped_texts.front(), current_content_marks[stream_index]);
        WriteTextClipGroup(buf, grouped_texts, group_sizes);
        std::vector<std::vector<TextClipSignature>> groups(
            matched.begin() + used_drop, matched.end());
        open_groups.push_back(OpenClipGroup{std::move(groups)});
        obj_index = run_end - 1;
        continue;
      }
      // No clip to preserve: fall through and write this text on its own.
      // The next text in the run gets its own chance on the next pass.
    }

    close_groups_not_in(buf, pPageObj);
    current_content_marks[stream_index] =
        ProcessContentMarks(buf, pPageObj, current_content_marks[stream_index]);
    ProcessPageObject(buf, pPageObj);
  }

  // Close any clip group still open at the end of its stream, before the
  // streams are finished (which closes the outer `q`).
  if (current_stream != CPDF_PageObject::kNoContentStream) {
    close_all_groups(current_stream);
  }

  // Finish dirty streams.
  for (int32_t dirty_stream : all_dirty_streams) {
    CFX_Matrix prev_ctm;
    CFX_Matrix ctm;
    bool affects_ctm;
    if (dirty_stream == 0) {
      // For the first stream, `prev_ctm` is the identity matrix.
      ctm = obj_holder_->GetCTMAtEndOfStream(dirty_stream);
      affects_ctm = !ctm.IsIdentity();
    } else if (dirty_stream > 0) {
      prev_ctm = obj_holder_->GetCTMAtEndOfStream(dirty_stream - 1);
      ctm = obj_holder_->GetCTMAtEndOfStream(dirty_stream);
      affects_ctm = prev_ctm != ctm;
    } else {
      CHECK_EQ(CPDF_PageObject::kNoContentStream, dirty_stream);
      // This is the last stream, so there is no subsequent stream that it can
      // affect.
      affects_ctm = false;
    }

    const bool is_empty = pdfium::Contains(empty_streams, dirty_stream);

    fxcrt::ostringstream* buf = &streams[dirty_stream];
    if (is_empty && !affects_ctm) {
      // Clear to show that this stream needs to be deleted.
      buf->str("");
      continue;
    }

    if (!is_empty) {
      FinishMarks(buf, current_content_marks[dirty_stream]);
    }

    // Return graphics to original state.
    *buf << "Q\n";

    if (affects_ctm) {
      // Update CTM so the next stream gets the expected value.
      CFX_Matrix ctm_difference = prev_ctm.GetInverse() * ctm;
      if (!ctm_difference.IsIdentity()) {
        WriteMatrix(*buf, ctm_difference) << " cm\n";
      }
    }
  }

  return streams;
}

void CPDF_PageContentGenerator::UpdateContentStreams(
    std::map<int32_t, fxcrt::ostringstream>&& new_stream_data) {
  CHECK(!new_stream_data.empty());

  // Make sure default graphics are created.
  default_graphics_name_ = GetOrCreateDefaultGraphics();

  CPDF_PageContentManager page_content_manager(obj_holder_, document_);
  for (auto& pair : new_stream_data) {
    int32_t stream_index = pair.first;
    fxcrt::ostringstream* buf = &pair.second;

    if (stream_index == CPDF_PageObject::kNoContentStream) {
      int new_stream_index =
          pdfium::checked_cast<int>(page_content_manager.AddStream(buf));
      UpdateStreamlessPageObjects(new_stream_index);
      continue;
    }

    if (page_content_manager.HasStreamAtIndex(stream_index)) {
      page_content_manager.UpdateStream(stream_index, buf);
    } else {
      page_content_manager.AddStream(buf);
    }
  }
}

void CPDF_PageContentGenerator::UpdateResourcesDict() {
  RetainPtr<CPDF_Dictionary> resources = obj_holder_->GetMutableResources();
  if (!resources) {
    return;
  }

  // Do not modify shared resource dictionaries. Give this page its own copy.
  if (IsPageResourceShared(document_, obj_holder_->GetDict(), resources)) {
    resources = pdfium::WrapRetain(resources->Clone()->AsMutableDictionary());
    const uint32_t clone_object_number =
        document_->AddIndirectObject(resources);
    obj_holder_->SetResources(resources);
    obj_holder_->GetMutableDict()->SetNewFor<CPDF_Reference>(
        pdfium::page_object::kResources, document_, clone_object_number);
  }

  // Even though `resources` itself is not shared, its dictionary entries may be
  // shared. Checked for that and clone those as well.
  CloneResourcesDictEntries(document_, resources);

  ResourcesMap seen_resources;
  for (auto& page_object : page_objects_) {
    if (!page_object->IsActive()) {
      continue;
    }
    RecordPageObjectResourceUsage(page_object, seen_resources);
  }
  if (!default_graphics_name_.IsEmpty()) {
    seen_resources["ExtGState"].insert(default_graphics_name_);
  }

  RemoveOrRestoreUnusedResources(std::move(resources), seen_resources,
                                 obj_holder_->all_removed_resources_map());
}

ByteString CPDF_PageContentGenerator::RealizeResource(
    const CPDF_Object* pResource,
    ByteStringView type) const {
  DCHECK(pResource);
  if (!obj_holder_->GetResources()) {
    obj_holder_->SetResources(document_->NewIndirect<CPDF_Dictionary>());
    obj_holder_->GetMutableDict()->SetNewFor<CPDF_Reference>(
        pdfium::page_object::kResources, document_,
        obj_holder_->GetResources()->GetObjNum());
  }

  RetainPtr<CPDF_Dictionary> resource_dict =
      obj_holder_->GetMutableResources()->GetOrCreateDictFor(type);
  const auto& all_removed_resources_map =
      obj_holder_->all_removed_resources_map();
  auto it = all_removed_resources_map.find(type);
  const CPDF_PageObjectHolder::RemovedResourceMap* removed_resource_map =
      it != all_removed_resources_map.end() ? &it->second : nullptr;
  ByteString name;
  int idnum = 1;
  while (true) {
    name = ByteString::Format("FX%c%d", type[0], idnum);
    // Avoid name collisions with existing `resource_dict` entries.
    if (resource_dict->KeyExist(name.AsStringView())) {
      idnum++;
      continue;
    }

    // Also avoid collisions with entries in `removed_resource_map`, since those
    // entries may move back into `resource_dict`.
    if (removed_resource_map && pdfium::Contains(*removed_resource_map, name)) {
      idnum++;
      continue;
    }

    resource_dict->SetNewFor<CPDF_Reference>(name, document_,
                                             pResource->GetObjNum());
    return name;
  }
}

bool CPDF_PageContentGenerator::ProcessPageObjects(fxcrt::ostringstream* buf) {
  bool bDirty = false;
  std::unique_ptr<const CPDF_ContentMarks> empty_content_marks =
      std::make_unique<CPDF_ContentMarks>();
  const CPDF_ContentMarks* content_marks = empty_content_marks.get();

  for (auto& pPageObj : page_objects_) {
    if (obj_holder_->IsPage() &&
        (!pPageObj->IsDirty() || !pPageObj->IsActive())) {
      continue;
    }

    bDirty = true;
    content_marks = ProcessContentMarks(buf, pPageObj, content_marks);
    ProcessPageObject(buf, pPageObj);
  }
  FinishMarks(buf, content_marks);
  return bDirty;
}

void CPDF_PageContentGenerator::UpdateStreamlessPageObjects(
    int new_content_stream_index) {
  for (auto& pPageObj : page_objects_) {
    if (!pPageObj->IsActive()) {
      continue;
    }

    if (pPageObj->GetContentStream() == CPDF_PageObject::kNoContentStream) {
      pPageObj->SetContentStream(new_content_stream_index);
    }
  }
}

const CPDF_ContentMarks* CPDF_PageContentGenerator::ProcessContentMarks(
    fxcrt::ostringstream* buf,
    const CPDF_PageObject* pPageObj,
    const CPDF_ContentMarks* pPrev) {
  const CPDF_ContentMarks* pNext = pPageObj->GetContentMarks();
  const size_t first_different = pPrev->FindFirstDifference(pNext);

  // Close all marks that are in prev but not in next.
  // Technically we should iterate backwards to close from the top to the
  // bottom, but since the EMC operators do not identify which mark they are
  // closing, it does not matter.
  for (size_t i = first_different; i < pPrev->CountItems(); ++i) {
    *buf << "EMC\n";
  }

  // Open all marks that are in next but not in prev.
  for (size_t i = first_different; i < pNext->CountItems(); ++i) {
    const CPDF_ContentMarkItem* item = pNext->GetItem(i);

    // Write mark tag.
    *buf << "/" << PDF_NameEncode(item->GetName()) << " ";

    // If there are no parameters, write a BMC (begin marked content) operator.
    if (item->GetParamType() == CPDF_ContentMarkItem::kNone) {
      *buf << "BMC\n";
      continue;
    }

    // If there are parameters, write properties, direct or indirect.
    switch (item->GetParamType()) {
      case CPDF_ContentMarkItem::kDirectDict: {
        CPDF_StringArchiveStream archive_stream(buf);
        item->GetParam()->WriteTo(&archive_stream, nullptr);
        *buf << " ";
        break;
      }
      case CPDF_ContentMarkItem::kPropertiesDict: {
        *buf << "/" << PDF_NameEncode(item->GetPropertyName()) << " ";
        break;
      }
      case CPDF_ContentMarkItem::kNone:
        NOTREACHED();
    }

    // Write BDC (begin dictionary content) operator.
    *buf << "BDC\n";
  }

  return pNext;
}

void CPDF_PageContentGenerator::FinishMarks(
    fxcrt::ostringstream* buf,
    const CPDF_ContentMarks* pContentMarks) {
  // Technically we should iterate backwards to close from the top to the
  // bottom, but since the EMC operators do not identify which mark they are
  // closing, it does not matter.
  for (size_t i = 0; i < pContentMarks->CountItems(); ++i) {
    *buf << "EMC\n";
  }
}

void CPDF_PageContentGenerator::ProcessPageObject(fxcrt::ostringstream* buf,
                                                  CPDF_PageObject* pPageObj) {
  if (CPDF_ImageObject* pImageObject = pPageObj->AsImage()) {
    ProcessImage(buf, pImageObject);
  } else if (CPDF_FormObject* pFormObj = pPageObj->AsForm()) {
    ProcessForm(buf, pFormObj);
  } else if (CPDF_PathObject* pPathObj = pPageObj->AsPath()) {
    ProcessPath(buf, pPathObj);
  } else if (CPDF_TextObject* pTextObj = pPageObj->AsText()) {
    ProcessText(buf, pTextObj);
  } else if (CPDF_ShadingObject* pShadingObj = pPageObj->AsShading()) {
    ProcessShading(buf, pShadingObj);
  }
  pPageObj->SetDirty(false);
}

void CPDF_PageContentGenerator::ProcessImage(fxcrt::ostringstream* buf,
                                             CPDF_ImageObject* pImageObj) {
  const CFX_Matrix& matrix = pImageObj->matrix();
  if ((matrix.a == 0 && matrix.b == 0) || (matrix.c == 0 && matrix.d == 0)) {
    return;
  }

  RetainPtr<CPDF_Image> pImage = pImageObj->GetImage();
  if (pImage->IsInline()) {
    ProcessInlineImage(buf, pImageObj);
    return;
  }

  RetainPtr<const CPDF_Stream> pStream = pImage->GetStream();
  if (!pStream) {
    return;
  }

  ProcessGraphics(buf, pImageObj);

  if (!matrix.IsIdentity()) {
    WriteMatrix(*buf, matrix) << " cm ";
  }

  bool bWasInline = pStream->IsInline();
  if (bWasInline) {
    pImage->ConvertStreamToIndirectObject();
  }

  ByteString name = RealizeResource(pStream, "XObject");
  pImageObj->SetResourceName(name);

  if (bWasInline) {
    auto* pPageData = CPDF_DocPageData::FromDocument(document_);
    pImageObj->SetImage(pPageData->GetImage(pStream->GetObjNum()));
  }

  *buf << "/" << PDF_NameEncode(name) << " Do";
  EndProcessGraphics(*buf);
}

// Writes an image that was parsed from an inline image back as an inline
// image: "BI" begins the image, followed by the entries of its dictionary;
// "ID" begins the image data, which is written exactly as it was read; "EI"
// ends the image.
void CPDF_PageContentGenerator::ProcessInlineImage(
    fxcrt::ostringstream* buf,
    CPDF_ImageObject* pImageObj) {
  RetainPtr<CPDF_Image> image = pImageObj->GetImage();
  RetainPtr<const CPDF_Stream> stream = image->GetStream();
  if (!stream) {
    return;
  }

  // Build the dictionary first: nothing is written when it holds something
  // that an inline image cannot express.
  fxcrt::ostringstream dict_buf;
  {
    CPDF_DictionaryLocker locker(stream->GetDict());
    for (const auto& it : locker) {
      const ByteString& key = it.first;
      // The parser adds /Length and /Subtype to make the dictionary look like
      // the one of an image XObject. They are not inline image entries.
      if (key == "Length" || key == "Subtype" || key == "Type") {
        continue;
      }
      dict_buf << " /"
               << PDF_NameEncode(AbbreviateInlineImageName(
                      kInlineImageKeyAbbreviations, key))
               << " ";
      const CPDF_Object* value = it.second.Get();
      bool written;
      if (key == "ColorSpace") {
        written = WriteInlineImageColorSpace(dict_buf, image.Get(), value);
      } else if (key == "Filter") {
        written = WriteInlineImageOperand(dict_buf, value,
                                          kInlineImageFilterAbbreviations, 0);
      } else {
        written = WriteInlineImageOperand(dict_buf, value, {}, 0);
      }
      if (!written) {
        return;
      }
    }
  }

  auto stream_acc = pdfium::MakeRetain<CPDF_StreamAcc>(std::move(stream));
  stream_acc->LoadAllDataRaw();
  pdfium::span<const char> data = pdfium::as_chars(stream_acc->GetSpan());

  ProcessGraphics(buf, pImageObj);
  const CFX_Matrix& matrix = pImageObj->matrix();
  if (!matrix.IsIdentity()) {
    WriteMatrix(*buf, matrix) << " cm ";
  }
  // Exactly one whitespace character separates "ID" from the image data.
  *buf << "BI" << dict_buf.str() << " ID\n";
  buf->write(data.data(), data.size());
  *buf << "\nEI";
  EndProcessGraphics(*buf);
}

bool CPDF_PageContentGenerator::WriteInlineImageColorSpace(
    fxcrt::ostringstream& buf,
    CPDF_Image* image,
    const CPDF_Object* color_space) const {
  // The color space is a resource: name it. The image named it as well when
  // the parser recorded the name; the dictionary itself may hold a copy of
  // the resource rather than the name.
  ByteString stored_name = image->GetInlineColorSpaceName();
  RetainPtr<const CPDF_Object> resource = image->GetInlineColorSpaceObject();
  if (stored_name.IsEmpty() || !resource) {
    if (color_space->IsName() || IsInlineImageColorSpace(color_space)) {
      // A device color space, an Indexed color space based on one, or the
      // name of a resource that did not exist when the image was parsed.
      return WriteInlineImageOperand(buf, color_space,
                                     kInlineImageColorSpaceAbbreviations, 0);
    }
    stored_name.clear();
    resource = color_space->GetDirect();
    if (!resource) {
      return false;
    }
  }

  RetainPtr<const CPDF_Dictionary> resources = obj_holder_->GetResources();
  RetainPtr<const CPDF_Dictionary> color_spaces =
      resources ? resources->GetDictFor("ColorSpace") : nullptr;
  ByteString name;
  if (color_spaces) {
    if (!stored_name.IsEmpty() && color_spaces->GetDirectObjectFor(
                                      stored_name.AsStringView()) == resource) {
      name = stored_name;
    } else {
      for (const ByteString& key : color_spaces->GetKeys()) {
        if (color_spaces->GetDirectObjectFor(key.AsStringView()) == resource) {
          name = key;
          break;
        }
      }
    }
  }
  if (name.IsEmpty()) {
    // The resource is not in the resources of this page (anymore), which
    // happens when the image moved here from another page. Add it.
    if (!document_) {
      return false;
    }
    if (!resource->GetObjNum()) {
      // Inline objects cannot be referenced from the resources dictionary.
      // Give the color space its own indirect object first.
      RetainPtr<CPDF_Object> copy = resource->Clone();
      document_->AddIndirectObject(copy);
      resource = std::move(copy);
    }
    name = RealizeResource(resource.Get(), "ColorSpace");
  }
  if (name != stored_name) {
    // Remember the name, so the next regeneration finds the resource.
    image->SetInlineColorSpaceResource(name, std::move(resource));
  }
  buf << "/" << PDF_NameEncode(name);
  return true;
}

void CPDF_PageContentGenerator::ProcessForm(fxcrt::ostringstream* buf,
                                            CPDF_FormObject* pFormObj) {
  CPDF_Form* form_xobject = pFormObj->form();
  if (form_xobject->HasDirtyStreams()) {
    // The Form XObject itself has modified content (e.g., an object was
    // removed). We need to regenerate its content stream before using it.
    CPDF_PageContentGenerator form_content_generator(form_xobject);
    form_content_generator.GenerateContent();
  }

  const CFX_Matrix& matrix = pFormObj->form_matrix();
  if ((matrix.a == 0 && matrix.b == 0) || (matrix.c == 0 && matrix.d == 0)) {
    return;
  }

  RetainPtr<const CPDF_Stream> pStream = pFormObj->form()->GetStream();
  CHECK(pStream);
  ByteString name = RealizeResource(pStream.Get(), "XObject");
  pFormObj->SetResourceName(name);

  ProcessGraphics(buf, pFormObj);

  if (!matrix.IsIdentity()) {
    WriteMatrix(*buf, matrix) << " cm ";
  }

  *buf << "/" << PDF_NameEncode(name) << " Do";
  EndProcessGraphics(*buf);
}

// "sh" paints a shading over the current clip, in the coordinate system the
// CTM sets up. It uses the alpha, blend mode and the rest of the general
// graphics state, but neither the current color nor the line parameters.
void CPDF_PageContentGenerator::ProcessShading(
    fxcrt::ostringstream* buf,
    CPDF_ShadingObject* pShadingObj) {
  ByteString name = GetShadingResourceName(pShadingObj);
  if (name.IsEmpty()) {
    return;
  }
  pShadingObj->SetResourceName(name);

  ProcessGraphics(buf, pShadingObj);

  const CFX_Matrix& matrix = pShadingObj->matrix();
  if (!matrix.IsIdentity()) {
    WriteMatrix(*buf, matrix) << " cm ";
  }

  *buf << "/" << PDF_NameEncode(name) << " sh";
  EndProcessGraphics(*buf);
}

ByteString CPDF_PageContentGenerator::GetShadingResourceName(
    const CPDF_ShadingObject* pShadingObj) const {
  const CPDF_ShadingPattern* pattern = pShadingObj->pattern();
  if (!pattern || !pattern->IsShadingObject()) {
    return ByteString();
  }
  // For a shading painted by "sh", the pattern object is the shading
  // dictionary or stream itself.
  RetainPtr<CPDF_Object> shading = pattern->GetPatternObj();
  if (!shading) {
    return ByteString();
  }

  return GetResourceName("Shading", std::move(shading),
                         pShadingObj->GetResourceName());
}

ByteString CPDF_PageContentGenerator::GetResourceName(
    ByteStringView type,
    RetainPtr<CPDF_Object> object,
    const ByteString& preferred_name) const {
  ByteString name = FindResourceName(type, object.Get(), preferred_name);
  if (!name.IsEmpty()) {
    return name;
  }

  // The object is not in the resources of this page or form (anymore),
  // which happens when the page object moved here from another page. Add it.
  if (!document_) {
    return ByteString();
  }
  if (!object->GetObjNum()) {
    // An object that is a direct object of another resources dictionary
    // cannot be referenced from here. Make it an indirect object; the
    // dictionary that holds it writes a reference from then on.
    document_->AddIndirectObject(object);
  }
  return RealizeResource(object.Get(), type);
}

ByteString CPDF_PageContentGenerator::FindResourceName(
    ByteStringView type,
    const CPDF_Object* object,
    const ByteString& preferred_name) const {
  RetainPtr<const CPDF_Dictionary> resources = obj_holder_->GetResources();
  RetainPtr<const CPDF_Dictionary> resources_of_type =
      resources ? resources->GetDictFor(type) : nullptr;
  if (!preferred_name.IsEmpty()) {
    if (resources_of_type &&
        resources_of_type->KeyExist(preferred_name.AsStringView())) {
      if (resources_of_type->GetDirectObjectFor(
              preferred_name.AsStringView()) == object) {
        return preferred_name;
      }
    } else {
      // A resource that was removed because nothing used it anymore is put
      // back under its name by UpdateResourcesDict() once it is in use
      // again.
      const auto& all_removed_resources_map =
          obj_holder_->all_removed_resources_map();
      auto it = all_removed_resources_map.find(type);
      if (it != all_removed_resources_map.end()) {
        auto removed_it = it->second.find(preferred_name);
        if (removed_it != it->second.end() && removed_it->second &&
            removed_it->second->GetDirect() == object) {
          return preferred_name;
        }
      }
    }
  }
  if (!resources_of_type) {
    return ByteString();
  }
  for (const ByteString& key : resources_of_type->GetKeys()) {
    if (resources_of_type->GetDirectObjectFor(key.AsStringView()) == object) {
      return key;
    }
  }
  return ByteString();
}

// Processing path construction with operators from Table 4.9 of PDF spec 1.7:
// "re" appends a rectangle (here, used only if the whole path is a rectangle)
// "m" moves current point to the given coordinates
// "l" creates a line from current point to the new point
// "c" adds a Bezier curve from current to last point, using the two other
// points as the Bezier control points
// Note: "l", "c" change the current point
// "h" closes the subpath (appends a line from current to starting point)
void CPDF_PageContentGenerator::ProcessPathPoints(fxcrt::ostringstream* buf,
                                                  CPDF_Path* pPath) {
  pdfium::span<const CFX_Path::Point> points = pPath->GetPoints();
  std::optional<CFX_PointF> corner = pPath->GetRectOppositeCorner();
  if (corner.has_value()) {
    CFX_PointF diff = corner.value() - points[0].point_;
    WritePoint(*buf, points[0].point_) << " ";
    WritePoint(*buf, diff) << " re";
    return;
  }
  for (size_t i = 0; i < points.size(); ++i) {
    if (i > 0) {
      *buf << " ";
    }

    WritePoint(*buf, points[i].point_);

    CFX_Path::Point::Type point_type = points[i].type_;
    if (point_type == CFX_Path::Point::Type::kMove) {
      *buf << " m";
    } else if (point_type == CFX_Path::Point::Type::kLine) {
      *buf << " l";
    } else if (point_type == CFX_Path::Point::Type::kBezier) {
      if (i + 2 >= points.size() ||
          !points[i].IsTypeAndOpen(CFX_Path::Point::Type::kBezier) ||
          !points[i + 1].IsTypeAndOpen(CFX_Path::Point::Type::kBezier) ||
          points[i + 2].type_ != CFX_Path::Point::Type::kBezier) {
        // If format is not supported, close the path and paint
        *buf << " h";
        break;
      }
      *buf << " ";
      WritePoint(*buf, points[i + 1].point_) << " ";
      WritePoint(*buf, points[i + 2].point_) << " c";
      i += 2;
    }
    if (points[i].close_figure_) {
      *buf << " h";
    }
  }
}

// Processing path painting with operators from Table 4.10 of PDF spec 1.7:
// Path painting operators: "S", "n", "B", "f", "B*", "f*", depending on
// the filling mode and whether we want stroking the path or not.
void CPDF_PageContentGenerator::ProcessPath(fxcrt::ostringstream* buf,
                                            CPDF_PathObject* pPathObj) {
  ProcessGraphics(buf, pPathObj);

  const CFX_Matrix& matrix = pPathObj->matrix();
  if (!matrix.IsIdentity()) {
    WriteMatrix(*buf, matrix) << " cm ";
  }

  ProcessPathPoints(buf, &pPathObj->path());

  if (pPathObj->has_no_filltype()) {
    *buf << (pPathObj->stroke() ? " S" : " n");
  } else if (pPathObj->has_winding_filltype()) {
    *buf << (pPathObj->stroke() ? " B" : " f");
  } else if (pPathObj->has_alternate_filltype()) {
    *buf << (pPathObj->stroke() ? " B*" : " f*");
  }
  EndProcessGraphics(*buf);
}

// This method supports color operators rg and RGB from Table 4.24 of PDF spec
// 1.7. A color will not be set if the colorspace is not DefaultRGB or the RGB
// values cannot be obtained. The method also adds an external graphics
// dictionary, as described in Section 4.3.4.
// "rg" sets the fill color, "RG" sets the stroke color (using DefaultRGB)
// "w" sets the stroke line width.
// "ca" sets the fill alpha, "CA" sets the stroke alpha.
// "W" and "W*" modify the clipping path using the nonzero winding rule and
// even-odd rules, respectively.
// "q" saves the graphics state, so that the settings can later be reversed
bool CPDF_PageContentGenerator::WriteColorToStream(
    fxcrt::ostringstream& buf,
    const CPDF_Color* color,
    ByteString colorspace_name,
    ByteString pattern_name,
    bool is_fill) {
  if (!color || color->IsNull()) {
    return false;
  }

  RetainPtr<CPDF_ColorSpace> colorspace = color->GetColorSpace();
  if (!colorspace) {
    return false;
  }

  if (color->IsPattern()) {
    return WritePatternColorToStream(buf, color, colorspace_name, pattern_name,
                                     is_fill);
  }

  pdfium::span<const float> components = color->GetComponents();
  if (components.size() < colorspace->ComponentCount()) {
    // A malformed color with fewer components than its color space needs.
    // There is nothing safe to write for it.
    return false;
  }

  switch (colorspace->GetFamily()) {
    case CPDF_ColorSpace::Family::kDeviceGray:
      WriteFloat(buf, components[0]) << (is_fill ? " g " : " G ");
      return true;
    case CPDF_ColorSpace::Family::kDeviceRGB:
      WriteFloat(buf, components[0]) << " ";
      WriteFloat(buf, components[1]) << " ";
      WriteFloat(buf, components[2]) << (is_fill ? " rg " : " RG ");
      return true;
    case CPDF_ColorSpace::Family::kDeviceCMYK:
      WriteFloat(buf, components[0]) << " ";
      WriteFloat(buf, components[1]) << " ";
      WriteFloat(buf, components[2]) << " ";
      WriteFloat(buf, components[3]) << (is_fill ? " k " : " K ");
      return true;
    case CPDF_ColorSpace::Family::kSeparation:
      // A Separation color space named /None paints nothing: its tint
      // transform is never applied (CPDF_SeparationCS::GetRGB() returns
      // nullopt), so writing the `cs`/`scn` operators back preserves the
      // blank area exactly. Converting it to an equivalent RGB color would
      // invent ink where the original has none.
      [[fallthrough]];
    case CPDF_ColorSpace::Family::kDeviceN:
    case CPDF_ColorSpace::Family::kIndexed:
      if (WriteNamedColorToStream(buf, colorspace, colorspace_name,
                                  components, /*use_names=*/true, is_fill)) {
        return true;
      }
      break;
    case CPDF_ColorSpace::Family::kCalGray:
    case CPDF_ColorSpace::Family::kCalRGB:
    case CPDF_ColorSpace::Family::kLab:
    case CPDF_ColorSpace::Family::kICCBased:
      if (WriteNamedColorToStream(buf, colorspace, colorspace_name,
                                  components, /*use_names=*/false, is_fill)) {
        return true;
      }
      break;
    default:
      break;
  }

  // A color space this generator does not know how to write, or one that
  // cannot be referred to by name: keep the previous behavior of writing
  // the equivalent RGB color, so the rendering is preserved even though the
  // color space itself is lost.
  std::optional<FX_RGB_STRUCT<float>> rgb = color->GetRGB();
  if (!rgb.has_value()) {
    return false;
  }
  WriteFloat(buf, rgb.value().red) << " ";
  WriteFloat(buf, rgb.value().green) << " ";
  WriteFloat(buf, rgb.value().blue) << (is_fill ? " rg " : " RG ");
  return true;
}

bool CPDF_PageContentGenerator::WriteNamedColorToStream(
    fxcrt::ostringstream& buf,
    RetainPtr<CPDF_ColorSpace> colorspace,
    ByteString stored_name,
    pdfium::span<const float> components,
    bool use_names,
    bool is_fill) {
  ByteString name = GetColorSpaceResourceName(colorspace, stored_name);
  if (name.IsEmpty()) {
    return false;
  }
  buf << "/" << PDF_NameEncode(name) << (is_fill ? " cs " : " CS ");
  const uint32_t count = colorspace->ComponentCount();
  for (uint32_t i = 0; i < count; ++i) {
    if (i > 0) {
      buf << " ";
    }
    WriteFloat(buf, components[i]);
  }
  if (use_names) {
    buf << (is_fill ? " scn " : " SCN ");
  } else {
    buf << (is_fill ? " sc " : " SC ");
  }
  return true;
}

ByteString CPDF_PageContentGenerator::GetColorSpaceResourceName(
    RetainPtr<CPDF_ColorSpace> colorspace,
    ByteString stored_name) const {
  if (!stored_name.IsEmpty()) {
    // A `cs` operand of one of the device names resolves through
    // FindColorSpace() to the default color space when one is defined, and
    // to the stock device space otherwise. The stored name is the name the
    // `cs` operator used, so writing it back selects the same color space.
    if (stored_name == "DeviceGray" || stored_name == "DeviceRGB" ||
        stored_name == "DeviceCMYK") {
      return stored_name;
    }
    if (stored_name == "Pattern" &&
        colorspace->GetFamily() == CPDF_ColorSpace::Family::kPattern &&
        !colorspace->GetBaseColorSpace()) {
      return stored_name;
    }
    RetainPtr<const CPDF_Dictionary> resources = obj_holder_->GetResources();
    if (colorspace->NameIsDefinedInResources(resources.Get(),
                                             stored_name.AsStringView())) {
      return stored_name;
    }
  }

  // Otherwise, realize the color space array as a new resource. Stock color
  // spaces have no array and cannot be realized; they are always selected by
  // their predefined name, handled above.
  RetainPtr<const CPDF_Array> array = colorspace->GetArray();
  if (!array || !document_) {
    return ByteString();
  }
  if (array->GetObjNum()) {
    return RealizeResource(array.Get(), "ColorSpace");
  }
  // Inline arrays cannot be referenced from the resources dictionary. Give
  // the array its own indirect object first.
  RetainPtr<CPDF_Object> array_copy = array->Clone();
  document_->AddIndirectObject(array_copy);
  return RealizeResource(array_copy.Get(), "ColorSpace");
}

bool CPDF_PageContentGenerator::WritePatternColorToStream(
    fxcrt::ostringstream& buf,
    const CPDF_Color* color,
    ByteString colorspace_name,
    ByteString pattern_name,
    bool is_fill) {
  RetainPtr<CPDF_Pattern> pattern = color->GetPattern();
  if (!pattern) {
    return false;
  }
  RetainPtr<CPDF_ColorSpace> colorspace = color->GetColorSpace();
  if (!colorspace) {
    return false;
  }

  // An uncolored tiling pattern takes the underlying color before the
  // pattern name; a colored tiling pattern and a shading pattern do not.
  // Resolve everything before writing, so a failure leaves `buf` untouched.
  pdfium::span<const float> underlay;
  size_t underlay_count = 0;
  CPDF_TilingPattern* tiling = pattern->AsTilingPattern();
  if (tiling && !tiling->colored()) {
    RetainPtr<CPDF_ColorSpace> base = colorspace->GetBaseColorSpace();
    underlay_count = base ? base->ComponentCount() : 0;
    underlay = color->GetPatternComponents();
    if (underlay.size() < underlay_count) {
      return false;
    }
  }

  ByteString cs_name = GetColorSpaceResourceName(colorspace, colorspace_name);
  if (cs_name.IsEmpty()) {
    return false;
  }
  ByteString name = GetPatternResourceName(pattern, pattern_name);
  if (name.IsEmpty()) {
    return false;
  }

  buf << "/" << PDF_NameEncode(cs_name) << (is_fill ? " cs " : " CS ");
  for (size_t i = 0; i < underlay_count; ++i) {
    if (i > 0) {
      buf << " ";
    }
    WriteFloat(buf, underlay[i]);
  }
  if (underlay_count > 0) {
    buf << " ";
  }
  buf << "/" << PDF_NameEncode(name) << (is_fill ? " scn " : " SCN ");
  return true;
}

ByteString CPDF_PageContentGenerator::GetPatternResourceName(
    RetainPtr<CPDF_Pattern> pattern,
    ByteString stored_name) const {
  RetainPtr<const CPDF_Dictionary> resources = obj_holder_->GetResources();
  if (resources) {
    RetainPtr<const CPDF_Dictionary> patterns =
        resources->GetDictFor("Pattern");
    if (patterns) {
      RetainPtr<const CPDF_Object> pattern_obj = pattern->GetPatternObj();
      RetainPtr<const CPDF_Object> pattern_direct =
          pattern_obj ? pattern_obj->GetDirect() : nullptr;
      if (pattern_direct) {
        if (!stored_name.IsEmpty()) {
          RetainPtr<const CPDF_Object> entry =
              patterns->GetDirectObjectFor(stored_name.AsStringView());
          if (entry && entry == pattern_direct) {
            return stored_name;
          }
        }
        for (const ByteString& key : patterns->GetKeys()) {
          RetainPtr<const CPDF_Object> entry =
              patterns->GetDirectObjectFor(key.AsStringView());
          if (entry && entry == pattern_direct) {
            return key;
          }
        }
      }
    }
  }
  if (!document_) {
    return ByteString();
  }
  RetainPtr<CPDF_Object> pattern_obj = pattern->GetPatternObj();
  if (!pattern_obj) {
    return ByteString();
  }
  if (!pattern_obj->GetObjNum()) {
    RetainPtr<CPDF_Object> pattern_copy = pattern_obj->Clone();
    document_->AddIndirectObject(pattern_copy);
    return RealizeResource(pattern_copy.Get(), "Pattern");
  }
  return RealizeResource(pattern_obj.Get(), "Pattern");
}

void CPDF_PageContentGenerator::ProcessGraphics(fxcrt::ostringstream* buf,
                                                CPDF_PageObject* pPageObj) {
  *buf << "q ";
  WriteGraphicsState(buf, pPageObj, /*include_clip_path=*/true);
}

void CPDF_PageContentGenerator::WriteClipPath(fxcrt::ostringstream* buf,
                                              const CPDF_ClipPath& clip_path) {
  if (!clip_path.HasRef()) {
    return;
  }
  for (size_t i = 0; i < clip_path.GetPathCount(); ++i) {
    CPDF_Path path = clip_path.GetPath(i);
    ProcessPathPoints(buf, &path);
    switch (clip_path.GetClipType(i)) {
      case CFX_FillRenderOptions::FillType::kWinding:
        *buf << " W ";
        break;
      case CFX_FillRenderOptions::FillType::kEvenOdd:
        *buf << " W* ";
        break;
      case CFX_FillRenderOptions::FillType::kNoFill:
        NOTREACHED();
    }

    // Use a no-op path-painting operator to terminate the path without
    // causing any marks to be placed on the page.
    *buf << "n ";
  }
}

void CPDF_PageContentGenerator::WriteGraphicsState(fxcrt::ostringstream* buf,
                                                   CPDF_PageObject* pPageObj,
                                                   bool include_clip_path) {
  // The graphics state parameter dictionaries the object was parsed with go
  // first. They are the only way to set the soft mask, the overprint
  // parameters and the like, and they may set the line parameters as well,
  // for which the operators below must have the last word.
  ParsedExtGStates parsed = WriteParsedExtGStates(buf, pPageObj);

  const CPDF_ColorState& color_state = pPageObj->color_state();
  WriteColorToStream(*buf, color_state.GetFillColor(),
                     color_state.GetFillColorSpaceName(),
                     color_state.GetFillPatternName(), /*is_fill=*/true);
  WriteColorToStream(*buf, color_state.GetStrokeColor(),
                     color_state.GetStrokeColorSpaceName(),
                     color_state.GetStrokePatternName(), /*is_fill=*/false);
  const CFX_GraphState& graph_state = pPageObj->graph_state();
  float line_width = graph_state.GetLineWidth();
  if (line_width != 1.0f || parsed.sets_line_width) {
    WriteFloat(*buf, line_width) << " w ";
  }
  CFX_GraphStateData::LineCap lineCap = graph_state.GetLineCap();
  if (lineCap != CFX_GraphStateData::LineCap::kButt || parsed.sets_line_cap) {
    *buf << static_cast<int>(lineCap) << " J ";
  }
  CFX_GraphStateData::LineJoin lineJoin = graph_state.GetLineJoin();
  if (lineJoin != CFX_GraphStateData::LineJoin::kMiter ||
      parsed.sets_line_join) {
    *buf << static_cast<int>(lineJoin) << " j ";
  }
  float miter_limit = graph_state.GetMiterLimit();
  if (miter_limit != kDefaultMiterLimit || parsed.sets_miter_limit) {
    WriteFloat(*buf, miter_limit) << " M ";
  }
  std::vector<float> dash_array = graph_state.GetLineDashArray();
  if (dash_array.size()) {
    *buf << "[";
    for (size_t i = 0; i < dash_array.size(); ++i) {
      if (i > 0) {
        *buf << " ";
      }
      WriteFloat(*buf, dash_array[i]);
    }
    *buf << "] ";
    WriteFloat(*buf, graph_state.GetLineDashPhase()) << " d ";
  } else if (parsed.sets_dash) {
    *buf << "[] 0 d ";
  }
  const CPDF_GeneralState& general_state = pPageObj->general_state();
  float flatness = general_state.GetFlatness();
  if (flatness != kDefaultFlatness || parsed.sets_flatness) {
    WriteFloat(*buf, flatness) << " i ";
  }

  if (include_clip_path) {
    WriteClipPath(buf, pPageObj->clip_path());
  }

  // Alpha and blend mode can be changed after parsing. When they are not
  // what the parsed dictionaries leave them at, set them with dictionaries
  // made for the purpose: the default graphics dictionary sets all three
  // parameters, and the dictionary that follows it the ones that are not
  // at their default value.
  GraphicsData graphD;
  graphD.fillAlpha = general_state.GetFillAlpha();
  graphD.strokeAlpha = general_state.GetStrokeAlpha();
  graphD.blendType = general_state.GetBlendType();
  if (graphD.fillAlpha != parsed.fill_alpha ||
      graphD.strokeAlpha != parsed.stroke_alpha ||
      graphD.blendType != parsed.blend_type) {
    if (parsed.fill_alpha != 1.0f || parsed.stroke_alpha != 1.0f ||
        parsed.blend_type != BlendMode::kNormal) {
      ByteString default_name = GetOrCreateDefaultGraphics();
      *buf << "/" << PDF_NameEncode(default_name) << " gs ";
      parsed.names.push_back(std::move(default_name));
    }
    if (graphD.fillAlpha != 1.0f || graphD.strokeAlpha != 1.0f ||
        graphD.blendType != BlendMode::kNormal) {
      ByteString name;
      std::optional<ByteString> maybe_name =
          obj_holder_->GraphicsMapSearch(graphD);
      if (maybe_name.has_value()) {
        name = std::move(maybe_name.value());
      } else {
        auto gsDict = document_->New<CPDF_Dictionary>();
        if (graphD.fillAlpha != 1.0f) {
          gsDict->SetNewFor<CPDF_Number>("ca", graphD.fillAlpha);
        }

        if (graphD.strokeAlpha != 1.0f) {
          gsDict->SetNewFor<CPDF_Number>("CA", graphD.strokeAlpha);
        }

        if (graphD.blendType != BlendMode::kNormal) {
          gsDict->SetNewFor<CPDF_Name>("BM", general_state.GetBlendMode());
        }
        document_->AddIndirectObject(gsDict);
        name = RealizeResource(std::move(gsDict), "ExtGState");
        obj_holder_->GraphicsMapInsert(graphD, name);
      }
      *buf << "/" << PDF_NameEncode(name) << " gs ";
      parsed.names.push_back(std::move(name));
    }
  }

  // Remember the names that were written: the resources they refer to are
  // in use. Most objects were written with the names they already had, and
  // share their state with other objects, so only touch the state when the
  // names changed.
  pdfium::span<const ByteString> stored_names =
      general_state.GetGraphicsResourceNames();
  if (!std::equal(parsed.names.begin(), parsed.names.end(),
                  stored_names.begin(), stored_names.end())) {
    pdfium::span<const RetainPtr<CPDF_Dictionary>> dicts =
        general_state.GetGraphicsResourceDicts();
    std::vector<RetainPtr<CPDF_Dictionary>> dicts_copy(dicts.begin(),
                                                       dicts.end());
    pPageObj->mutable_general_state().SetGraphicsResources(
        std::move(parsed.names), std::move(dicts_copy));
  }
}

CPDF_PageContentGenerator::ParsedExtGStates
CPDF_PageContentGenerator::WriteParsedExtGStates(
    fxcrt::ostringstream* buf,
    const CPDF_PageObject* pPageObj) const {
  ParsedExtGStates result;
  const CPDF_GeneralState& general_state = pPageObj->general_state();
  pdfium::span<const RetainPtr<CPDF_Dictionary>> dicts =
      general_state.GetGraphicsResourceDicts();
  if (dicts.empty()) {
    return result;
  }
  pdfium::span<const ByteString> stored_names =
      general_state.GetGraphicsResourceNames();
  CHECK_GE(stored_names.size(), dicts.size());

  // The dictionary that sets the soft mask may need the matrix that was
  // current when it was applied to be the current one again.
  const std::optional<size_t> soft_mask_index =
      FindSoftMaskDictThatNeedsMatrix(pPageObj);
  CFX_Matrix soft_mask_matrix;
  if (soft_mask_index.has_value()) {
    soft_mask_matrix = *general_state.GetSMaskMatrix();
  }

  // Follows what the dictionaries do to the parameters that can be changed
  // after parsing, the same way CPDF_AllStates::ProcessExtGS() does.
  CPDF_GeneralState applied;
  for (size_t i = 0; i < dicts.size(); ++i) {
    const RetainPtr<CPDF_Dictionary>& dict = dicts[i];
    ByteString name = GetResourceName("ExtGState", dict, stored_names[i]);
    if (name.IsEmpty()) {
      // Keep the names lined up with the dictionaries.
      result.names.push_back(stored_names[i]);
      continue;
    }
    if (i == soft_mask_index) {
      WriteMatrix(*buf, soft_mask_matrix) << " cm ";
    }
    *buf << "/" << PDF_NameEncode(name) << " gs ";
    if (i == soft_mask_index) {
      WriteMatrix(*buf, soft_mask_matrix.GetInverse()) << " cm ";
    }
    result.names.push_back(std::move(name));

    if (RetainPtr<const CPDF_Object> alpha = dict->GetDirectObjectFor("ca")) {
      applied.SetFillAlpha(std::clamp(alpha->GetNumber(), 0.0f, 1.0f));
    }
    if (RetainPtr<const CPDF_Object> alpha = dict->GetDirectObjectFor("CA")) {
      applied.SetStrokeAlpha(std::clamp(alpha->GetNumber(), 0.0f, 1.0f));
    }
    if (RetainPtr<const CPDF_Object> mode = dict->GetDirectObjectFor("BM")) {
      const CPDF_Array* array = mode->AsArray();
      applied.SetBlendMode(array ? array->GetByteStringAt(0)
                                 : mode->GetString());
    }
    result.sets_line_width |= dict->KeyExist("LW");
    result.sets_line_cap |= dict->KeyExist("LC");
    result.sets_line_join |= dict->KeyExist("LJ");
    result.sets_miter_limit |= dict->KeyExist("ML");
    result.sets_dash |= dict->KeyExist("D");
    result.sets_flatness |= dict->KeyExist("FL");
  }
  result.fill_alpha = applied.GetFillAlpha();
  result.stroke_alpha = applied.GetStrokeAlpha();
  result.blend_type = applied.GetBlendType();
  return result;
}

void CPDF_PageContentGenerator::ProcessDefaultGraphics(
    fxcrt::ostringstream* buf) {
  *buf << "0 0 0 RG 0 0 0 rg 1 w "
       << static_cast<int>(CFX_GraphStateData::LineCap::kButt) << " J "
       << static_cast<int>(CFX_GraphStateData::LineJoin::kMiter) << " j\n";
  default_graphics_name_ = GetOrCreateDefaultGraphics();
  *buf << "/" << PDF_NameEncode(default_graphics_name_) << " gs ";
}

ByteString CPDF_PageContentGenerator::GetOrCreateDefaultGraphics() const {
  GraphicsData defaultGraphics;
  defaultGraphics.fillAlpha = 1.0f;
  defaultGraphics.strokeAlpha = 1.0f;
  defaultGraphics.blendType = BlendMode::kNormal;

  std::optional<ByteString> maybe_name =
      obj_holder_->GraphicsMapSearch(defaultGraphics);
  if (maybe_name.has_value()) {
    return maybe_name.value();
  }

  auto gsDict = document_->New<CPDF_Dictionary>();
  gsDict->SetNewFor<CPDF_Number>("ca", defaultGraphics.fillAlpha);
  gsDict->SetNewFor<CPDF_Number>("CA", defaultGraphics.strokeAlpha);
  gsDict->SetNewFor<CPDF_Name>("BM", "Normal");
  document_->AddIndirectObject(gsDict);
  ByteString name = RealizeResource(std::move(gsDict), "ExtGState");
  obj_holder_->GraphicsMapInsert(defaultGraphics, name);
  return name;
}

// This method adds text to the buffer, BT begins the text object, ET ends it.
// Tm sets the text matrix (allows positioning and transforming text).
// Tf sets the font name (from Font in Resources) and font size.
// Tr sets the text rendering mode.
// TJ sets the actual text, <####...> is used when specifying charcodes.
void CPDF_PageContentGenerator::ProcessText(fxcrt::ostringstream* buf,
                                            CPDF_TextObject* pTextObj) {
  if (!CanWriteText(pTextObj)) {
    // Type3 fonts cannot be written back: their glyphs are arbitrary content
    // streams. Skip the object without emitting the "q" and "BT" that would
    // otherwise leave the stream unbalanced and swallow everything after it.
    return;
  }

  ProcessGraphics(buf, pTextObj);
  *buf << "BT ";
  WriteTextContent(buf, pTextObj, /*force_text_matrix=*/false);
  *buf << " ET";
  EndProcessGraphics(*buf);
}

bool CPDF_PageContentGenerator::CanWriteText(const CPDF_TextObject* pTextObj) {
  RetainPtr<CPDF_Font> font(pTextObj->GetFont());
  if (!font) {
    // Falls back to Helvetica, which can be written.
    return true;
  }
  return font->IsType1Font() || font->IsTrueTypeFont() || font->IsCIDFont();
}

bool CPDF_PageContentGenerator::WriteTextContent(fxcrt::ostringstream* buf,
                                                 CPDF_TextObject* pTextObj,
                                                 bool force_text_matrix) {
  RetainPtr<CPDF_Font> font(pTextObj->GetFont());
  if (!font) {
    font = CPDF_Font::GetStockFont(document_, "Helvetica");
  }

  FontData data;
  const CPDF_FontEncoding* pEncoding = nullptr;
  if (font->IsType1Font()) {
    data.type = "Type1";
    pEncoding = font->AsType1Font()->GetEncoding();
  } else if (font->IsTrueTypeFont()) {
    data.type = "TrueType";
    pEncoding = font->AsTrueTypeFont()->GetEncoding();
  } else if (font->IsCIDFont()) {
    data.type = "Type0";
  } else {
    return false;
  }
  data.baseFont = font->GetBaseFontName();

  const CFX_Matrix& matrix = pTextObj->GetTextMatrix();
  if (force_text_matrix || !matrix.IsIdentity()) {
    WriteMatrix(*buf, matrix) << " Tm ";
  }

  ByteString dict_name;
  std::optional<ByteString> maybe_name = obj_holder_->FontsMapSearch(data);
  if (maybe_name.has_value()) {
    dict_name = std::move(maybe_name.value());
  } else {
    RetainPtr<const CPDF_Object> pIndirectFont = font->GetFontDict();
    if (pIndirectFont->IsInline()) {
      // In this case we assume it must be a standard font
      auto font_dict = document_->New<CPDF_Dictionary>();
      font_dict->SetNewFor<CPDF_Name>("Type", "Font");
      font_dict->SetNewFor<CPDF_Name>("Subtype", data.type);
      font_dict->SetNewFor<CPDF_Name>("BaseFont", data.baseFont);
      if (pEncoding) {
        font_dict->SetFor("Encoding",
                          pEncoding->Realize(document_->GetByteStringPool()));
      }
      document_->AddIndirectObject(font_dict);
      pIndirectFont = std::move(font_dict);
    }
    dict_name = RealizeResource(std::move(pIndirectFont), "Font");
    obj_holder_->FontsMapInsert(data, dict_name);
  }
  pTextObj->SetResourceName(dict_name);

  *buf << "/" << PDF_NameEncode(dict_name) << " ";
  WriteFloat(*buf, pTextObj->GetFontSize()) << " Tf ";

  // Tc and Tw are part of the graphics state, so the enclosing q/Q pair
  // restores them. Only write non-default values to avoid bloating the stream.
  float char_space = pTextObj->text_state().GetCharSpace();
  if (char_space != 0.0f) {
    WriteFloat(*buf, char_space) << " Tc ";
  }
  float word_space = pTextObj->text_state().GetWordSpace();
  if (word_space != 0.0f) {
    WriteFloat(*buf, word_space) << " Tw ";
  }

  *buf << static_cast<int>(pTextObj->GetTextRenderMode()) << " Tr ";

  *buf << "[";
  ByteString text;
  for (auto [char_code, char_kerning] :
       fxcrt::Zip(pTextObj->GetCharCodes(), pTextObj->GetCharKernings())) {
    font->AppendChar(&text, char_code);
    if (char_kerning != 0) {
      *buf << PDF_HexEncodeString(text.AsStringView()) << " ";
      text.clear();
      WriteFloat(*buf, char_kerning) << " ";
    }
  }
  if (!text.IsEmpty()) {
    *buf << PDF_HexEncodeString(text.AsStringView());
  }
  *buf << "] TJ";
  return true;
}

void CPDF_PageContentGenerator::WriteTextClipGroup(
    fxcrt::ostringstream* buf,
    const std::vector<CPDF_TextObject*>& texts,
    const std::vector<size_t>& group_sizes) {
  // One `q` for the whole clip, the path clip of the first text, then one
  // BT...ET per original BT...ET group. The `q` is left open: the caller
  // closes it once no later object needs the clip anymore.
  *buf << "q ";
  WriteClipPath(buf, texts.front()->clip_path());
  size_t index = 0;
  for (size_t group_size : group_sizes) {
    *buf << "BT ";
    for (size_t i = 0; i < group_size; ++i) {
      CPDF_TextObject* text = texts[index];
      if (i > 0) {
        // Separate from the previous text's TJ, which has no trailing
        // space.
        *buf << " ";
      }
      // The graphics state of each text (color, line, ExtGState) goes
      // inside the BT...ET, as the original file does; the path clip is
      // already written above and is the same for all texts in the group.
      WriteGraphicsState(buf, text, /*include_clip_path=*/false);
      WriteTextContent(buf, text, /*force_text_matrix=*/index > 0);
      ++index;
    }
    // The newline separates this ET from the next BT...ET, the next object,
    // or the `Q` that closes the group.
    *buf << " ET\n";
  }
}
