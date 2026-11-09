// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_EDIT_CPDF_PAGECONTENTGENERATOR_H_
#define CORE_FPDFAPI_EDIT_CPDF_PAGECONTENTGENERATOR_H_

#include <stdint.h>

#include <map>
#include <vector>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/unowned_ptr.h"

class CPDF_ClipPath;
class CPDF_Color;
class CPDF_ColorSpace;
class CPDF_ContentMarks;
class CPDF_Document;
class CPDF_FormObject;
class CPDF_ImageObject;
class CPDF_Object;
class CPDF_PageObject;
class CPDF_PageObjectHolder;
class CPDF_Path;
class CPDF_PathObject;
class CPDF_Pattern;
class CPDF_TextObject;

class CPDF_PageContentGenerator {
 public:
  explicit CPDF_PageContentGenerator(CPDF_PageObjectHolder* pObjHolder);
  ~CPDF_PageContentGenerator();

  void GenerateContent();
  bool ProcessPageObjects(fxcrt::ostringstream* buf);

 private:
  friend class CPDFPageContentGeneratorTest;

  void ProcessPageObject(fxcrt::ostringstream* buf, CPDF_PageObject* pPageObj);
  void ProcessPathPoints(fxcrt::ostringstream* buf, CPDF_Path* pPath);
  void ProcessPath(fxcrt::ostringstream* buf, CPDF_PathObject* pPathObj);
  void ProcessForm(fxcrt::ostringstream* buf, CPDF_FormObject* pFormObj);
  void ProcessImage(fxcrt::ostringstream* buf, CPDF_ImageObject* pImageObj);
  void ProcessGraphics(fxcrt::ostringstream* buf, CPDF_PageObject* pPageObj);
  void ProcessDefaultGraphics(fxcrt::ostringstream* buf);
  void ProcessText(fxcrt::ostringstream* buf, CPDF_TextObject* pTextObj);
  ByteString GetOrCreateDefaultGraphics() const;
  ByteString RealizeResource(const CPDF_Object* pResource,
                             ByteStringView type) const;
  // Writes the fill or stroke color of `color` to `buf`, using the color
  // space the color was set in rather than an equivalent RGB color.
  // `colorspace_name`/`pattern_name` are the resource names that selected
  // the color space/pattern, as recorded by the parser. Returns whether
  // anything was written.
  bool WriteColorToStream(fxcrt::ostringstream& buf,
                          const CPDF_Color* color,
                          ByteString colorspace_name,
                          ByteString pattern_name,
                          bool is_fill);
  // Writes a color in a named color space: `/Name cs`/`CS` followed by the
  // components and `sc`/`SC` (`use_names` selects `scn`/`SCN` instead, for
  // the color spaces whose colors need names). Returns false when the color
  // space cannot be referred to by name.
  bool WriteNamedColorToStream(fxcrt::ostringstream& buf,
                               RetainPtr<CPDF_ColorSpace> colorspace,
                               ByteString stored_name,
                               pdfium::span<const float> components,
                               bool use_names,
                               bool is_fill);
  // Writes a pattern color: the pattern color space, then the underlying
  // color for an uncolored tiling pattern, then `/Name scn`/`SCN`.
  bool WritePatternColorToStream(fxcrt::ostringstream& buf,
                                 const CPDF_Color* color,
                                 ByteString colorspace_name,
                                 ByteString pattern_name,
                                 bool is_fill);
  // Returns the name to use for `colorspace` in a `cs`/`CS` operator: the
  // stored name when it still refers to this color space, a newly realized
  // name for the color space array otherwise. Empty when the color space
  // cannot be referred to by name at all.
  ByteString GetColorSpaceResourceName(RetainPtr<CPDF_ColorSpace> colorspace,
                                       ByteString stored_name) const;
  // Returns the name to use for `pattern` in an `scn`/`SCN` operator.
  ByteString GetPatternResourceName(RetainPtr<CPDF_Pattern> pattern,
                                    ByteString stored_name) const;
  // Writes the path clip of `clip_path`: each path with its W/W* operator,
  // terminated by the no-op `n` painting operator.
  void WriteClipPath(fxcrt::ostringstream* buf,
                     const CPDF_ClipPath& clip_path);
  // Writes the graphics state of `pPageObj` (color, line width/cap/join/
  // dash, clip path, ExtGState) without the enclosing `q`. ProcessGraphics()
  // is `q` + WriteGraphicsState().
  void WriteGraphicsState(fxcrt::ostringstream* buf,
                          CPDF_PageObject* pPageObj,
                          bool include_clip_path);
  // Writes the text content of `pTextObj` (text matrix, font, Tc/Tw/Tr and
  // the TJ) without the enclosing `BT...ET` or `q...Q`. `force_text_matrix`
  // writes the Tm even when it is the identity matrix: a text that follows
  // another text in the same BT...ET must reset the text position, it cannot
  // rely on where the previous text ended. Returns false when the font
  // cannot be written back (Type3); callers check CanWriteText() first.
  bool WriteTextContent(fxcrt::ostringstream* buf,
                        CPDF_TextObject* pTextObj,
                        bool force_text_matrix);
  // Whether the font of `pTextObj` can be written back. Type3 fonts cannot:
  // their glyphs are arbitrary content streams. A missing font falls back
  // to Helvetica, which can be written.
  static bool CanWriteText(const CPDF_TextObject* pTextObj);
  // Writes `texts` (in order) as the clip of the objects that follow them:
  // one `q`, the path clip of the first text, then one `BT...ET` per entry
  // of `group_sizes` (each group is the texts of one original BT...ET, so
  // their glyphs are unioned within a group and the groups intersect, as
  // the renderer does). The `q` is left open: the caller closes it once no
  // later object needs the clip anymore.
  void WriteTextClipGroup(fxcrt::ostringstream* buf,
                          const std::vector<CPDF_TextObject*>& texts,
                          const std::vector<size_t>& group_sizes);
  const CPDF_ContentMarks* ProcessContentMarks(fxcrt::ostringstream* buf,
                                               const CPDF_PageObject* pPageObj,
                                               const CPDF_ContentMarks* pPrev);
  void FinishMarks(fxcrt::ostringstream* buf,
                   const CPDF_ContentMarks* pContentMarks);

  // Returns a map from content stream index to new stream data. Unmodified
  // streams are not touched.
  std::map<int32_t, fxcrt::ostringstream> GenerateModifiedStreams();

  // For each entry in `new_stream_data`, adds the string buffer to the page's
  // content stream.
  void UpdateContentStreams(
      std::map<int32_t, fxcrt::ostringstream>&& new_stream_data);

  // Sets the stream index of all page objects with stream index ==
  // |CPDF_PageObject::kNoContentStream|. These are new objects that had not
  // been parsed from or written to any content stream yet.
  void UpdateStreamlessPageObjects(int new_content_stream_index);

  // Updates the resource dictionary for `obj_holder_` to account for all the
  // changes.
  void UpdateResourcesDict();

  UnownedPtr<CPDF_PageObjectHolder> const obj_holder_;
  UnownedPtr<CPDF_Document> const document_;
  std::vector<UnownedPtr<CPDF_PageObject>> page_objects_;
  ByteString default_graphics_name_;
};

#endif  // CORE_FPDFAPI_EDIT_CPDF_PAGECONTENTGENERATOR_H_
