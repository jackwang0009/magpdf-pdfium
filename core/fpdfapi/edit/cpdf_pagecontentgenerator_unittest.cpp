// Copyright 2017 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/fpdfapi/edit/cpdf_pagecontentgenerator.h"

#include <iterator>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/page/cpdf_textstate.h"
#include "core/fpdfapi/page/test_with_page_module.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_test_document.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxge/cfx_fillrenderoptions.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

class CPDFPageContentGeneratorTest : public TestWithPageModule {
 protected:
  void TestProcessPath(CPDF_PageContentGenerator* pGen,
                       fxcrt::ostringstream* buf,
                       CPDF_PathObject* pPathObj) {
    pGen->ProcessPath(buf, pPathObj);
  }

  RetainPtr<const CPDF_Dictionary> TestGetResource(
      CPDF_PageContentGenerator* pGen,
      ByteStringView type,
      ByteStringView name) {
    RetainPtr<const CPDF_Dictionary> pResources =
        pGen->obj_holder_->GetResources();
    return pResources->GetDictFor(type)->GetDictFor(name);
  }

  void TestProcessText(CPDF_PageContentGenerator* pGen,
                       fxcrt::ostringstream* buf,
                       CPDF_TextObject* pTextObj) {
    pGen->ProcessText(buf, pTextObj);
  }

  void TestWriteTextClipGroup(CPDF_PageContentGenerator* pGen,
                              fxcrt::ostringstream* buf,
                              const std::vector<CPDF_TextObject*>& texts,
                              const std::vector<size_t>& group_sizes) {
    pGen->WriteTextClipGroup(buf, texts, group_sizes);
  }

  void TestProcessPageObject(CPDF_PageContentGenerator* pGen,
                             fxcrt::ostringstream* buf,
                             CPDF_PageObject* pPageObj) {
    pGen->ProcessPageObject(buf, pPageObj);
  }

  // Adds a page to `doc` with `contents` as its content stream and
  // `resources` (may be null) as its resources, and parses it.
  RetainPtr<CPDF_Page> CreateParsedPage(CPDF_TestDocument* doc,
                                        RetainPtr<CPDF_Dictionary> resources,
                                        ByteStringView contents) {
    RetainPtr<CPDF_Dictionary> page_dict(
        doc->CreateNewPage(doc->GetPageCount()));
    if (resources) {
      page_dict->SetFor("Resources", std::move(resources));
    }
    auto stream = doc->NewIndirect<CPDF_Stream>(
        DataVector<uint8_t>(contents.unsigned_span().begin(),
                            contents.unsigned_span().end()),
        doc->New<CPDF_Dictionary>());
    page_dict->SetNewFor<CPDF_Reference>("Contents", doc, stream->GetObjNum());
    auto page = pdfium::MakeRetain<CPDF_Page>(doc, std::move(page_dict));
    page->ParseContent();
    return page;
  }
};

TEST_F(CPDFPageContentGeneratorTest, ProcessRect) {
  auto pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->set_stroke(true);
  pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kEvenOdd);
  pPathObj->path().AppendRect(10, 5, 13, 30);

  auto dummy_page_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  auto pTestPage = pdfium::MakeRetain<CPDF_Page>(nullptr, dummy_page_dict);
  CPDF_PageContentGenerator generator(pTestPage.Get());
  fxcrt::ostringstream buf;
  TestProcessPath(&generator, &buf, pPathObj.get());
  EXPECT_EQ("q 10 5 3 25 re B* Q\n", ByteString(buf));

  pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->path().AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kMove);
  pPathObj->path().AppendPoint(CFX_PointF(5.2f, 0),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(5.2f, 3.78f),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPointAndClose(CFX_PointF(0, 3.78f),
                                       CFX_Path::Point::Type::kLine);
  buf.str("");
  TestProcessPath(&generator, &buf, pPathObj.get());
  EXPECT_EQ("q 0 0 5.2 3.78 re n Q\n", ByteString(buf));

  // A rectangle with a duplicate second point, so its third point is not the
  // opposite corner.
  pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->path().AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kMove);
  pPathObj->path().AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(2, 0), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(2, 1), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(0, 1), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kLine);
  buf.str("");
  TestProcessPath(&generator, &buf, pPathObj.get());
  EXPECT_EQ("q 0 0 2 1 re n Q\n", ByteString(buf));

  // A rectangle with an extra point in the middle of its first edge, so its
  // third point is not the opposite corner either.
  pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->path().AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kMove);
  pPathObj->path().AppendPoint(CFX_PointF(50, 0), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(100, 0),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(100, 100),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(0, 100),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPointAndClose(CFX_PointF(0, 0),
                                       CFX_Path::Point::Type::kLine);
  buf.str("");
  TestProcessPath(&generator, &buf, pPathObj.get());
  EXPECT_EQ("q 0 0 100 100 re n Q\n", ByteString(buf));

  // Same as above, but closed without repeating the first point.
  pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->path().AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kMove);
  pPathObj->path().AppendPoint(CFX_PointF(50, 0), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(100, 0),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(100, 100),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPointAndClose(CFX_PointF(0, 100),
                                       CFX_Path::Point::Type::kLine);
  buf.str("");
  TestProcessPath(&generator, &buf, pPathObj.get());
  EXPECT_EQ("q 0 0 100 100 re n Q\n", ByteString(buf));
}

TEST_F(CPDFPageContentGeneratorTest, Bug937) {
  static const std::vector<float> rgb = {0.000000000000000000001f, 0.7f, 0.35f};
  RetainPtr<CPDF_ColorSpace> pCS =
      CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceRGB);
  {
    auto pPathObj = std::make_unique<CPDF_PathObject>();
    pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);

    // Test code in ProcessPath that generates re operator
    pPathObj->path().AppendRect(0.000000000000000000001,
                                0.000000000000000000001, 100, 100);

    pPathObj->mutable_color_state().SetFillColor(pCS, rgb);
    pPathObj->mutable_color_state().SetStrokeColor(pCS, rgb);
    pPathObj->mutable_graph_state().SetLineWidth(200000000000000000001.0);
    pPathObj->Transform(CFX_Matrix(1, 0, 0, 1, 0.000000000000000000001,
                                   200000000000000.000002));

    auto dummy_page_dict = pdfium::MakeRetain<CPDF_Dictionary>();
    auto pTestPage = pdfium::MakeRetain<CPDF_Page>(nullptr, dummy_page_dict);
    CPDF_PageContentGenerator generator(pTestPage.Get());
    fxcrt::ostringstream buf;
    TestProcessPath(&generator, &buf, pPathObj.get());
    EXPECT_EQ(
        "q .000000000000000000001 .7 .35 rg "
        ".000000000000000000001 .7 .35 RG "
        "200000000000000000000 w 1 0 0 1 .000000000000000000001 "
        "200000000000000 cm .000000000000000000001 "
        ".000000000000000000001 100 100 re f Q\n",
        ByteString(buf));
  }

  {
    // Test code in ProcessPath that handles bezier operator
    auto pPathObj = std::make_unique<CPDF_PathObject>();
    pPathObj->mutable_color_state().SetFillColor(pCS, rgb);
    pPathObj->mutable_color_state().SetStrokeColor(pCS, rgb);
    pPathObj->mutable_graph_state().SetLineWidth(2.000000000000000000001);
    pPathObj->Transform(CFX_Matrix(1, 0, 0, 1, 432, 500000000000000.000002));

    pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
    pPathObj->path().AppendPoint(CFX_PointF(0.000000000000000000001f, 4.67f),
                                 CFX_Path::Point::Type::kMove);
    pPathObj->path().AppendPoint(
        CFX_PointF(0.000000000000000000001, 100000000000000.000002),
        CFX_Path::Point::Type::kLine);
    pPathObj->path().AppendPoint(CFX_PointF(0.0000000000001f, 3.15f),
                                 CFX_Path::Point::Type::kBezier);
    pPathObj->path().AppendPoint(CFX_PointF(3.57f, 2.98f),
                                 CFX_Path::Point::Type::kBezier);
    pPathObj->path().AppendPointAndClose(
        CFX_PointF(53.4f, 5000000000000000000.00000000000000004),
        CFX_Path::Point::Type::kBezier);
    auto dummy_page_dict = pdfium::MakeRetain<CPDF_Dictionary>();
    auto pTestPage = pdfium::MakeRetain<CPDF_Page>(nullptr, dummy_page_dict);
    CPDF_PageContentGenerator generator(pTestPage.Get());
    fxcrt::ostringstream buf;

    TestProcessPath(&generator, &buf, pPathObj.get());
    EXPECT_EQ(
        "q .000000000000000000001 .7 .35 rg "
        ".000000000000000000001 .7 .35 RG 2 w 1 0 0 1 432 "
        "500000000000000 cm .000000000000000000001 4.67 m "
        ".000000000000000000001 100000000000000 l "
        ".0000000000001 3.15 3.57 2.98 53.4 "
        "5000000000000000000 c h f Q\n",
        ByteString(buf));
  }
}

TEST_F(CPDFPageContentGeneratorTest, ProcessPath) {
  auto pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
  pPathObj->path().AppendPoint(CFX_PointF(3.102f, 4.67f),
                               CFX_Path::Point::Type::kMove);
  pPathObj->path().AppendPoint(CFX_PointF(5.45f, 0.29f),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(4.24f, 3.15f),
                               CFX_Path::Point::Type::kBezier);
  pPathObj->path().AppendPoint(CFX_PointF(4.65f, 2.98f),
                               CFX_Path::Point::Type::kBezier);
  pPathObj->path().AppendPoint(CFX_PointF(3.456f, 0.24f),
                               CFX_Path::Point::Type::kBezier);
  pPathObj->path().AppendPoint(CFX_PointF(10.6f, 11.15f),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(11, 12.5f),
                               CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPoint(CFX_PointF(11.46f, 12.67f),
                               CFX_Path::Point::Type::kBezier);
  pPathObj->path().AppendPoint(CFX_PointF(11.84f, 12.96f),
                               CFX_Path::Point::Type::kBezier);
  pPathObj->path().AppendPointAndClose(CFX_PointF(12, 13.64f),
                                       CFX_Path::Point::Type::kBezier);

  auto dummy_page_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  auto pTestPage = pdfium::MakeRetain<CPDF_Page>(nullptr, dummy_page_dict);
  CPDF_PageContentGenerator generator(pTestPage.Get());
  fxcrt::ostringstream buf;
  TestProcessPath(&generator, &buf, pPathObj.get());
  EXPECT_EQ(
      "q 3.102 4.67 m 5.45 .29 l 4.24 "
      "3.15 4.65 2.98 3.456 .24 c 10.6 "
      "11.15 l 11 12.5 l 11.46 12.67 11.84 12.96 12 13.64 c h f"
      " Q\n",
      ByteString(buf));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessGraphics) {
  auto pPathObj = std::make_unique<CPDF_PathObject>();
  pPathObj->set_stroke(true);
  pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
  pPathObj->path().AppendPoint(CFX_PointF(1, 2), CFX_Path::Point::Type::kMove);
  pPathObj->path().AppendPoint(CFX_PointF(3, 4), CFX_Path::Point::Type::kLine);
  pPathObj->path().AppendPointAndClose(CFX_PointF(5, 6),
                                       CFX_Path::Point::Type::kLine);

  static const std::vector<float> rgb = {0.5f, 0.7f, 0.35f};
  RetainPtr<CPDF_ColorSpace> pCS =
      CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceRGB);
  pPathObj->mutable_color_state().SetFillColor(pCS, rgb);

  static const std::vector<float> rgb2 = {1, 0.9f, 0};
  pPathObj->mutable_color_state().SetStrokeColor(pCS, rgb2);
  pPathObj->mutable_general_state().SetFillAlpha(0.5f);
  pPathObj->mutable_general_state().SetStrokeAlpha(0.8f);

  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  RetainPtr<CPDF_Dictionary> pPageDict(doc->CreateNewPage(0));
  auto pTestPage = pdfium::MakeRetain<CPDF_Page>(doc.get(), pPageDict);
  CPDF_PageContentGenerator generator(pTestPage.Get());
  fxcrt::ostringstream buf;
  TestProcessPath(&generator, &buf, pPathObj.get());
  ByteString path_string(buf);

  // Color RGB values used are integers divided by 255.
  const ByteStringView kExpectedStringStart = "q .5 .7 .35 rg 1 .9 0 RG /";
  const ByteStringView kExpectedStringEnd = " gs 1 2 m 3 4 l 5 6 l h B Q\n";
  const size_t kExpectedStringMinLength =
      kExpectedStringStart.GetLength() + kExpectedStringEnd.GetLength();
  EXPECT_EQ(kExpectedStringStart,
            path_string.First(kExpectedStringStart.GetLength()));
  EXPECT_EQ(kExpectedStringEnd,
            path_string.Last(kExpectedStringEnd.GetLength()));
  ASSERT_GT(path_string.GetLength(), kExpectedStringMinLength);
  RetainPtr<const CPDF_Dictionary> external_gs =
      TestGetResource(&generator, "ExtGState",
                      path_string.AsStringView().Substr(
                          kExpectedStringStart.GetLength(),
                          path_string.GetLength() - kExpectedStringMinLength));
  ASSERT_TRUE(external_gs);
  EXPECT_EQ(0.5f, external_gs->GetFloatFor("ca"));
  EXPECT_EQ(0.8f, external_gs->GetFloatFor("CA"));

  // Same path, now with a stroke.
  pPathObj->mutable_graph_state().SetLineWidth(10.5f);
  buf.str("");
  TestProcessPath(&generator, &buf, pPathObj.get());
  const ByteStringView kExpectedStringStart2 =
      "q .5 .7 .35 rg 1 .9 0 RG 10.5 w /";
  ByteString path_string2(buf);
  EXPECT_EQ(kExpectedStringStart2,
            path_string2.First(kExpectedStringStart2.GetLength()));
  EXPECT_EQ(kExpectedStringEnd,
            path_string2.Last(kExpectedStringEnd.GetLength()));

  // Compare with the previous (should use same dictionary for gs)
  const size_t kExpectedStringsLengthDifference =
      kExpectedStringStart2.GetLength() - kExpectedStringStart.GetLength();
  EXPECT_EQ(path_string.GetLength() + kExpectedStringsLengthDifference,
            path_string2.GetLength());
  EXPECT_EQ(
      path_string.Substr(kExpectedStringStart.GetLength(),
                         path_string.GetLength() - kExpectedStringMinLength),
      path_string2.Substr(kExpectedStringStart2.GetLength(),
                          path_string2.GetLength() - kExpectedStringMinLength -
                              kExpectedStringsLengthDifference));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessStandardText) {
  // Checking font whose font dictionary is not yet indirect object.
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  RetainPtr<CPDF_Dictionary> pPageDict(doc->CreateNewPage(0));
  auto pTestPage = pdfium::MakeRetain<CPDF_Page>(doc.get(), pPageDict);
  CPDF_PageContentGenerator generator(pTestPage.Get());
  auto pTextObj = std::make_unique<CPDF_TextObject>();
  pTextObj->mutable_text_state().SetFont(
      CPDF_Font::GetStockFont(doc.get(), "Times-Roman"));
  pTextObj->mutable_text_state().SetFontSize(10.0f);

  static const std::vector<float> rgb = {0.5f, 0.7f, 0.35f};
  RetainPtr<CPDF_ColorSpace> pCS =
      CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceRGB);
  pTextObj->mutable_color_state().SetFillColor(pCS, rgb);

  static const std::vector<float> rgb2 = {1, 0.9f, 0};
  pTextObj->mutable_color_state().SetStrokeColor(pCS, rgb2);
  pTextObj->mutable_general_state().SetFillAlpha(0.5f);
  pTextObj->mutable_general_state().SetStrokeAlpha(0.8f);
  pTextObj->Transform(CFX_Matrix(1, 0, 0, 1, 100, 100));
  pTextObj->SetText("Hello World");
  fxcrt::ostringstream buf;
  TestProcessText(&generator, &buf, pTextObj.get());
  ByteString text_string(buf);
  auto first_resource_at = text_string.Find('/');
  ASSERT_TRUE(first_resource_at.has_value());
  first_resource_at = first_resource_at.value() + 1;
  auto second_resource_at = text_string.ReverseFind('/');
  ASSERT_TRUE(second_resource_at.has_value());
  second_resource_at = second_resource_at.value() + 1;
  ByteString first_string = text_string.First(first_resource_at.value());
  ByteString mid_string = text_string.Substr(
      first_resource_at.value(),
      second_resource_at.value() - first_resource_at.value());
  ByteString last_string = text_string.Substr(second_resource_at.value());
  // q and Q must be outside the BT .. ET operations
  const ByteString kCompareString1 = "q .5 .7 .35 rg 1 .9 0 RG /";
  // Color RGB values used are integers divided by 255.
  const ByteString kCompareString2 = " gs BT 1 0 0 1 100 100 Tm /";
  const ByteString kCompareString3 =
      " 10 Tf 0 Tr [<48656C6C6F20576F726C64>] TJ ET Q\n";
  EXPECT_LT(kCompareString1.GetLength() + kCompareString2.GetLength() +
                kCompareString3.GetLength(),
            text_string.GetLength());
  EXPECT_EQ(kCompareString1, first_string.First(kCompareString1.GetLength()));
  EXPECT_EQ(kCompareString2, mid_string.Last(kCompareString2.GetLength()));
  EXPECT_EQ(kCompareString3, last_string.Last(kCompareString3.GetLength()));
  RetainPtr<const CPDF_Dictionary> external_gs = TestGetResource(
      &generator, "ExtGState",
      mid_string.AsStringView().First(mid_string.GetLength() -
                                      kCompareString2.GetLength()));
  ASSERT_TRUE(external_gs);
  EXPECT_EQ(0.5f, external_gs->GetFloatFor("ca"));
  EXPECT_EQ(0.8f, external_gs->GetFloatFor("CA"));
  RetainPtr<const CPDF_Dictionary> font_dict = TestGetResource(
      &generator, "Font",
      last_string.AsStringView().First(last_string.GetLength() -
                                       kCompareString3.GetLength()));
  ASSERT_TRUE(font_dict);
  EXPECT_EQ("Font", font_dict->GetNameFor("Type"));
  EXPECT_EQ("Type1", font_dict->GetNameFor("Subtype"));
  EXPECT_EQ("Times-Roman", font_dict->GetNameFor("BaseFont"));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessText) {
  // Checking font whose font dictionary is already an indirect object.
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  RetainPtr<CPDF_Dictionary> pPageDict(doc->CreateNewPage(0));
  auto pTestPage = pdfium::MakeRetain<CPDF_Page>(doc.get(), pPageDict);
  CPDF_PageContentGenerator generator(pTestPage.Get());

  fxcrt::ostringstream buf;
  {
    // Set the text object font and text
    auto pTextObj = std::make_unique<CPDF_TextObject>();
    auto dict = doc->NewIndirect<CPDF_Dictionary>();
    dict->SetNewFor<CPDF_Name>("Type", "Font");
    dict->SetNewFor<CPDF_Name>("Subtype", "TrueType");

    RetainPtr<CPDF_Font> font = CPDF_Font::GetStockFont(doc.get(), "Arial");
    dict->SetNewFor<CPDF_Name>("BaseFont", font->GetBaseFontName());

    auto pDesc = doc->NewIndirect<CPDF_Dictionary>();
    pDesc->SetNewFor<CPDF_Name>("Type", "FontDescriptor");
    pDesc->SetNewFor<CPDF_Name>("FontName", font->GetBaseFontName());
    dict->SetNewFor<CPDF_Reference>("FontDescriptor", doc.get(),
                                    pDesc->GetObjNum());

    pTextObj->mutable_text_state().SetFont(
        CPDF_DocPageData::FromDocument(doc.get())->GetFont(dict));
    pTextObj->mutable_text_state().SetFontSize(15.5f);
    pTextObj->SetText("I am indirect");
    pTextObj->SetTextRenderMode(TextRenderingMode::MODE_FILL_CLIP);

    // Add a clipping path.
    auto pPath = std::make_unique<CPDF_Path>();
    pPath->AppendPoint(CFX_PointF(0, 0), CFX_Path::Point::Type::kMove);
    pPath->AppendPoint(CFX_PointF(5, 0), CFX_Path::Point::Type::kLine);
    pPath->AppendPoint(CFX_PointF(5, 4), CFX_Path::Point::Type::kLine);
    pPath->AppendPointAndClose(CFX_PointF(0, 4), CFX_Path::Point::Type::kLine);
    CPDF_ClipPath& clip_path = pTextObj->mutable_clip_path();
    clip_path.Emplace();
    clip_path.AppendPath(*pPath, CFX_FillRenderOptions::FillType::kEvenOdd);

    TestProcessText(&generator, &buf, pTextObj.get());
  }

  ByteString text_string(buf);
  auto first_resource_at = text_string.Find('/');
  ASSERT_TRUE(first_resource_at.has_value());
  first_resource_at = first_resource_at.value() + 1;
  ByteString first_string = text_string.First(first_resource_at.value());
  ByteString last_string = text_string.Substr(first_resource_at.value());
  // q and Q must be outside the BT .. ET operations
  ByteString compare_string1 = "q 0 0 5 4 re W* n BT /";
  ByteString compare_string2 =
      " 15.5 Tf 4 Tr [<4920616D20696E646972656374>] TJ ET Q\n";
  EXPECT_LT(compare_string1.GetLength() + compare_string2.GetLength(),
            text_string.GetLength());
  EXPECT_EQ(compare_string1, text_string.First(compare_string1.GetLength()));
  EXPECT_EQ(compare_string2, text_string.Last(compare_string2.GetLength()));
  RetainPtr<const CPDF_Dictionary> font_dict = TestGetResource(
      &generator, "Font",
      text_string.AsStringView().Substr(compare_string1.GetLength(),
                                        text_string.GetLength() -
                                            compare_string1.GetLength() -
                                            compare_string2.GetLength()));
  ASSERT_TRUE(font_dict);
  EXPECT_TRUE(font_dict->GetObjNum());
  EXPECT_EQ("Font", font_dict->GetNameFor("Type"));
  EXPECT_EQ("TrueType", font_dict->GetNameFor("Subtype"));
  EXPECT_EQ("Helvetica", font_dict->GetNameFor("BaseFont"));
  RetainPtr<const CPDF_Dictionary> font_desc =
      font_dict->GetDictFor("FontDescriptor");
  ASSERT_TRUE(font_desc);
  EXPECT_TRUE(font_desc->GetObjNum());
  EXPECT_EQ("FontDescriptor", font_desc->GetNameFor("Type"));
  EXPECT_EQ("Helvetica", font_desc->GetNameFor("FontName"));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessTextCharAndWordSpace) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  RetainPtr<CPDF_Dictionary> pPageDict(doc->CreateNewPage(0));
  auto pTestPage = pdfium::MakeRetain<CPDF_Page>(doc.get(), pPageDict);
  CPDF_PageContentGenerator generator(pTestPage.Get());

  // Non-default Tc and Tw must be written before the text showing operator.
  {
    auto pTextObj = std::make_unique<CPDF_TextObject>();
    pTextObj->mutable_text_state().SetFont(
        CPDF_Font::GetStockFont(doc.get(), "Times-Roman"));
    pTextObj->mutable_text_state().SetFontSize(10.0f);
    pTextObj->mutable_text_state().SetCharSpace(0.5f);
    pTextObj->mutable_text_state().SetWordSpace(-1.25f);
    pTextObj->SetText("ab");

    fxcrt::ostringstream buf;
    TestProcessText(&generator, &buf, pTextObj.get());
    ByteString text_string(buf);
    // The q/Q pair encloses the whole text object, so Tc and Tw are restored
    // and cannot leak into the objects that follow.
    EXPECT_TRUE(text_string.Contains(" 10 Tf .5 Tc -1.25 Tw 0 Tr "));
    EXPECT_EQ('q', text_string[0]);
    EXPECT_EQ("Q\n", text_string.Last(2));
    EXPECT_EQ('q', text_string[0]);
    EXPECT_EQ("Q\n", text_string.Last(2));
  }

  // Default values are not written.
  {
    auto pTextObj = std::make_unique<CPDF_TextObject>();
    pTextObj->mutable_text_state().SetFont(
        CPDF_Font::GetStockFont(doc.get(), "Times-Roman"));
    pTextObj->mutable_text_state().SetFontSize(10.0f);
    pTextObj->SetText("ab");

    fxcrt::ostringstream buf;
    TestProcessText(&generator, &buf, pTextObj.get());
    ByteString text_string(buf);
    EXPECT_FALSE(text_string.Contains(" Tc "));
    EXPECT_FALSE(text_string.Contains(" Tw "));
    EXPECT_TRUE(text_string.Contains(" 10 Tf 0 Tr "));
  }
}

TEST_F(CPDFPageContentGeneratorTest, ProcessColorSpaces) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  RetainPtr<CPDF_Dictionary> pPageDict(doc->CreateNewPage(0));

  // Lab colour space in the page resources, as the parser would leave it:
  // [/Lab << /WhitePoint [0.9505 1 1.089] /Range [-100 100 -100 100] >>].
  RetainPtr<CPDF_Array> lab_array = doc->New<CPDF_Array>();
  lab_array->Append(pdfium::MakeRetain<CPDF_Name>(nullptr, "Lab"));
  RetainPtr<CPDF_Dictionary> lab_dict = doc->New<CPDF_Dictionary>();
  RetainPtr<CPDF_Array> white_point = doc->New<CPDF_Array>();
  white_point->AppendNew<CPDF_Number>(0.9505f);
  white_point->AppendNew<CPDF_Number>(1.0f);
  white_point->AppendNew<CPDF_Number>(1.089f);
  lab_dict->SetFor("WhitePoint", std::move(white_point));
  RetainPtr<CPDF_Array> range = doc->New<CPDF_Array>();
  range->AppendNew<CPDF_Number>(-100.0f);
  range->AppendNew<CPDF_Number>(100.0f);
  range->AppendNew<CPDF_Number>(-100.0f);
  range->AppendNew<CPDF_Number>(100.0f);
  lab_dict->SetFor("Range", std::move(range));
  lab_array->Append(std::move(lab_dict));
  const uint32_t lab_obj_num = doc->AddIndirectObject(lab_array);

  RetainPtr<CPDF_Dictionary> resources =
      pPageDict->SetNewFor<CPDF_Dictionary>("Resources");
  RetainPtr<CPDF_Dictionary> color_spaces =
      resources->SetNewFor<CPDF_Dictionary>("ColorSpace");
  color_spaces->SetNewFor<CPDF_Reference>("CSlab", doc.get(), lab_obj_num);

  auto pTestPage = pdfium::MakeRetain<CPDF_Page>(doc.get(), pPageDict);
  pTestPage->SetResources(resources);
  CPDF_PageContentGenerator generator(pTestPage.Get());

  std::set<const CPDF_Object*> visited;
  RetainPtr<CPDF_ColorSpace> lab =
      CPDF_ColorSpace::Load(doc.get(), lab_array.Get(), &visited);
  ASSERT_TRUE(lab);

  // Lab fill reuses its stored resource name; DeviceCMYK stroke is written
  // with the raw components.
  {
    auto pPathObj = std::make_unique<CPDF_PathObject>();
    pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
    pPathObj->path().AppendRect(10, 5, 13, 30);
    static const std::vector<float> lab_values = {50.0f, 80.0f, 0.0f};
    pPathObj->mutable_color_state().SetFillColor(lab, lab_values, "CSlab");
    static const std::vector<float> cmyk_values = {0.0f, 0.5f, 1.0f, 0.0f};
    pPathObj->mutable_color_state().SetStrokeColor(
        CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceCMYK),
        cmyk_values);
    fxcrt::ostringstream buf;
    TestProcessPath(&generator, &buf, pPathObj.get());
    ByteString path_string(buf);
    EXPECT_EQ(ByteString("q /CSlab cs 50 80 0 sc 0 .5 1 0 K 10 5 3 25 re f Q\n"),
              path_string);
  }

  // DeviceCMYK fill writes k; DeviceGray fill writes g.
  {
    auto pPathObj = std::make_unique<CPDF_PathObject>();
    pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
    pPathObj->path().AppendRect(10, 5, 13, 30);
    static const std::vector<float> cmyk_values = {0.1f, 0.2f, 0.3f, 0.4f};
    pPathObj->mutable_color_state().SetFillColor(
        CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceCMYK),
        cmyk_values);
    fxcrt::ostringstream buf;
    TestProcessPath(&generator, &buf, pPathObj.get());
    ByteString path_string(buf);
    EXPECT_EQ(ByteString("q .1 .2 .3 .4 k 10 5 3 25 re f Q\n"), path_string);
  }
  {
    auto pPathObj = std::make_unique<CPDF_PathObject>();
    pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
    pPathObj->path().AppendRect(10, 5, 13, 30);
    static const std::vector<float> gray_values = {0.5f};
    pPathObj->mutable_color_state().SetFillColor(
        CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceGray),
        gray_values);
    fxcrt::ostringstream buf;
    TestProcessPath(&generator, &buf, pPathObj.get());
    ByteString path_string(buf);
    EXPECT_EQ(ByteString("q .5 g 10 5 3 25 re f Q\n"), path_string);
  }

  // Separation /None is written back faithfully (and leaves no ink), not
  // dropped: dropping it would paint a black block where the original is
  // blank. The array is realised under the first free FXC name.
  {
    RetainPtr<CPDF_Array> sep_array = doc->New<CPDF_Array>();
    sep_array->Append(pdfium::MakeRetain<CPDF_Name>(nullptr, "Separation"));
    sep_array->Append(pdfium::MakeRetain<CPDF_Name>(nullptr, "None"));
    sep_array->Append(pdfium::MakeRetain<CPDF_Name>(nullptr, "DeviceCMYK"));
    const uint32_t sep_obj_num = doc->AddIndirectObject(sep_array);
    (void)sep_obj_num;
    std::set<const CPDF_Object*> visited_sep;
    RetainPtr<CPDF_ColorSpace> sep_none =
        CPDF_ColorSpace::Load(doc.get(), sep_array.Get(), &visited_sep);
    ASSERT_TRUE(sep_none);
    auto pPathObj = std::make_unique<CPDF_PathObject>();
    pPathObj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
    pPathObj->path().AppendRect(10, 5, 13, 30);
    static const std::vector<float> none_values = {1.0f};
    pPathObj->mutable_color_state().SetFillColor(sep_none, none_values,
                                                 "CSn");
    fxcrt::ostringstream buf;
    TestProcessPath(&generator, &buf, pPathObj.get());
    ByteString path_string(buf);
    EXPECT_EQ(ByteString("q /FXC1 cs 1 scn 10 5 3 25 re f Q\n"), path_string);
  }
}

TEST_F(CPDFPageContentGeneratorTest, ProcessTextClipGroup) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  RetainPtr<CPDF_Dictionary> pPageDict(doc->CreateNewPage(0));
  auto pTestPage = pdfium::MakeRetain<CPDF_Page>(doc.get(), pPageDict);
  CPDF_PageContentGenerator generator(pTestPage.Get());

  auto make_clip_text = [&](const char* text, float x) {
    auto pTextObj = std::make_unique<CPDF_TextObject>();
    pTextObj->mutable_text_state().SetFont(
        CPDF_Font::GetStockFont(doc.get(), "Times-Roman"));
    pTextObj->mutable_text_state().SetFontSize(22.0f);
    pTextObj->mutable_text_state().SetTextMode(
        TextRenderingMode::MODE_CLIP);
    pTextObj->Transform(CFX_Matrix(1, 0, 0, 1, x, 25));
    pTextObj->SetText(text);
    return pTextObj;
  };
  auto text1 = make_clip_text("Two", 20);
  auto text2 = make_clip_text("Blocks", 110);

  // Two texts from one BT...ET stay in one BT...ET.
  fxcrt::ostringstream buf;
  TestWriteTextClipGroup(&generator, &buf, {text1.get(), text2.get()}, {2});
  ByteString text_string(buf);
  // One `q` for the whole clip, left open: no closing `Q`.
  EXPECT_EQ('q', text_string[0]);
  EXPECT_FALSE(text_string.Contains(" Q\n"));
  EXPECT_TRUE(text_string.Contains("BT "));
  EXPECT_TRUE(text_string.Contains(" 7 Tr [<54776F>] TJ"));
  EXPECT_TRUE(text_string.Contains(" 7 Tr [<426C6F636B73>] TJ"));
  auto first_tj = text_string.Find(" 7 Tr [<54776F>] TJ");
  auto second_tj = text_string.Find(" 7 Tr [<426C6F636B73>] TJ");
  ASSERT_TRUE(first_tj.has_value());
  ASSERT_TRUE(second_tj.has_value());
  EXPECT_LT(first_tj.value(), second_tj.value());
  // Both texts share one font resource at 22pt.
  EXPECT_TRUE(text_string.Contains(" 22 Tf "));
  auto first_slash = text_string.Find('/');
  auto last_slash = text_string.ReverseFind('/');
  ASSERT_TRUE(first_slash.has_value());
  ASSERT_TRUE(last_slash.has_value());
  EXPECT_LT(first_slash.value(), last_slash.value());
  ByteString first_font = text_string.Substr(first_slash.value() + 1);
  ByteString last_font = text_string.Substr(last_slash.value() + 1);
  auto first_space = first_font.Find(" ");
  auto last_space = last_font.Find(" ");
  ASSERT_TRUE(first_space.has_value());
  ASSERT_TRUE(last_space.has_value());
  EXPECT_EQ(first_font.First(first_space.value()),
            last_font.First(last_space.value()));
  // The second text needs its own text matrix: it cannot rely on where the
  // first text ended.
  EXPECT_TRUE(text_string.Contains("1 0 0 1 110 25 Tm "));

  // Two texts from two BT...ETs become two BT...ETs under the same `q`.
  buf.str("");
  TestWriteTextClipGroup(&generator, &buf, {text1.get(), text2.get()},
                         {1, 1});
  text_string = ByteString(buf);
  EXPECT_EQ('q', text_string[0]);
  EXPECT_FALSE(text_string.Contains(" Q\n"));
  EXPECT_TRUE(text_string.Contains(" ET\nBT "));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessInlineImage) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  // An Indexed color space that is a direct object in the resources: the
  // parser copies it into the dictionary of the inline image that names it.
  auto resources = doc->New<CPDF_Dictionary>();
  auto color_spaces = resources->SetNewFor<CPDF_Dictionary>("ColorSpace");
  auto indexed = color_spaces->SetNewFor<CPDF_Array>("CSidx");
  indexed->AppendNew<CPDF_Name>("Indexed");
  indexed->AppendNew<CPDF_Name>("DeviceRGB");
  indexed->AppendNew<CPDF_Number>(1);
  static constexpr uint8_t kPalette[] = {0xff, 0x8b, 0x00, 0x00, 0x44, 0xaa};
  indexed->AppendNew<CPDF_String>(kPalette, CPDF_String::DataType::kIsHex);

  // In order: ASCIIHex data, unfiltered data that contains "EI ", an image
  // mask, the Indexed color space resource, a filter chain with decode
  // parameters, and an Indexed color space spelled out in the image.
  static constexpr char kContents[] =
      "q 60 0 0 40 20 140 cm BI /W 4 /H 4 /CS /G /BPC 8 /F /AHx ID\n"
      "004080FF4080FF0080FF0040FF004080 >\nEI Q\n"
      "q 60 0 0 40 100 140 cm BI /W 2 /H 2 /CS /RGB /BPC 8 ID\n"
      "\xff\x01\x01\x01\xff\x01"
      "EI \x01\x01\xff\nEI Q\n"
      "q 0 0.4 0 rg 60 0 0 40 180 140 cm BI /W 8 /H 8 /IM true /BPC 1 "
      "/D [0 1] /I true /F /AHx ID\nFF81BDA5A5BD81FF >\nEI Q\n"
      "q 60 0 0 40 20 60 cm BI /W 4 /H 4 /CS /CSidx /BPC 1 /F /AHx ID\n"
      "A050A050 >\nEI Q\n"
      "q 60 0 0 40 100 60 cm BI /W 2 /H 1 /CS /G /BPC 8 /F [/AHx /RL] "
      "/DP [null << /K 1 >>] ID\n01FFFF80 >\nEI Q\n"
      "q 60 0 0 40 180 60 cm BI /W 4 /H 4 /CS [/I /RGB 1 <FF8B000044AA>] "
      "/BPC 1 /F /AHx ID\nA050A050 >\nEI Q\n";
  RetainPtr<CPDF_Page> page = CreateParsedPage(doc.get(), resources, kContents);
  ASSERT_EQ(6u, page->GetPageObjectCount());

  CPDF_PageContentGenerator generator(page.Get());
  auto generate = [&](size_t index) {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator, &buf, page->GetPageObjectByIndex(index));
    return ByteString(buf);
  };

  // The dictionary entries come out in the order of their full names, with
  // the abbreviations the parser expanded, and without the /Length and
  // /Subtype the parser added. The image data is untouched.
  EXPECT_EQ(
      "q 60 0 0 40 20 140 cm BI /BPC 8 /CS /G /F /AHx /H 4 /W 4 ID\n"
      "004080FF4080FF0080FF0040FF004080 >\nEI Q\n",
      generate(0));
  EXPECT_EQ(
      "q 60 0 0 40 100 140 cm BI /BPC 8 /CS /RGB /H 2 /W 2 ID\n"
      "\xff\x01\x01\x01\xff\x01"
      "EI \x01\x01\xff\nEI Q\n",
      generate(1));
  // An image mask is painted with the fill color.
  EXPECT_EQ(
      "q 0 .4 0 rg 0 G 60 0 0 40 180 140 cm BI /BPC 1 /D [0 1] /F /AHx /H 8 "
      "/IM true /I true /W 8 ID\nFF81BDA5A5BD81FF >\nEI Q\n",
      generate(2));
  // The color space resource is named again, not spelled out.
  EXPECT_EQ(
      "q 60 0 0 40 20 60 cm BI /BPC 1 /CS /CSidx /F /AHx /H 4 /W 4 ID\n"
      "A050A050 >\nEI Q\n",
      generate(3));
  EXPECT_EQ(
      "q 60 0 0 40 100 60 cm BI /BPC 8 /CS /G /DP [null <</K 1 >>] "
      "/F [/AHx /RL] /H 1 /W 2 ID\n01FFFF80 >\nEI Q\n",
      generate(4));
  EXPECT_EQ(
      "q 60 0 0 40 180 60 cm BI /BPC 1 /CS [/I /RGB 1 <FF8B000044AA>] "
      "/F /AHx /H 4 /W 4 ID\nA050A050 >\nEI Q\n",
      generate(5));

  // No resource was added for any of them.
  EXPECT_EQ(1u, color_spaces->size());

  // Parsing the generated inline images gives the same images again.
  ByteString regenerated;
  for (size_t i = 0; i < 6; ++i) {
    regenerated += generate(i);
  }
  RetainPtr<CPDF_Page> page2 =
      CreateParsedPage(doc.get(), resources, regenerated.AsStringView());
  ASSERT_EQ(6u, page2->GetPageObjectCount());
  CPDF_PageContentGenerator generator2(page2.Get());
  ByteString regenerated2;
  for (size_t i = 0; i < 6; ++i) {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator2, &buf, page2->GetPageObjectByIndex(i));
    regenerated2 += ByteString(buf);
  }
  EXPECT_EQ(regenerated, regenerated2);
}

TEST_F(CPDFPageContentGeneratorTest, ProcessInlineImageOnAnotherPage) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  // A Lab color space cannot be spelled out in an inline image, only named.
  auto resources = doc->New<CPDF_Dictionary>();
  auto color_spaces = resources->SetNewFor<CPDF_Dictionary>("ColorSpace");
  auto lab = color_spaces->SetNewFor<CPDF_Array>("CSlab");
  lab->AppendNew<CPDF_Name>("Lab");
  auto lab_dict = lab->AppendNew<CPDF_Dictionary>();
  auto white_point = lab_dict->SetNewFor<CPDF_Array>("WhitePoint");
  white_point->AppendNew<CPDF_Number>(0.9505f);
  white_point->AppendNew<CPDF_Number>(1.0f);
  white_point->AppendNew<CPDF_Number>(1.089f);

  static constexpr char kContents[] =
      "q 60 0 0 40 20 60 cm BI /W 1 /H 1 /CS /CSlab /BPC 8 /F /AHx ID\n"
      "80FF00 >\nEI Q\n";
  RetainPtr<CPDF_Page> page = CreateParsedPage(doc.get(), resources, kContents);
  ASSERT_EQ(1u, page->GetPageObjectCount());
  CPDF_PageObject* image = page->GetPageObjectByIndex(0);

  // On its own page, the image names the color space as it did.
  {
    CPDF_PageContentGenerator generator(page.Get());
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator, &buf, image);
    EXPECT_EQ(
        "q 60 0 0 40 20 60 cm BI /BPC 8 /CS /CSlab /F /AHx /H 1 /W 1 ID\n"
        "80FF00 >\nEI Q\n",
        ByteString(buf));
  }

  // On a page without that resource, the color space becomes a resource of
  // that page under a new name.
  RetainPtr<CPDF_Page> other_page = CreateParsedPage(doc.get(), nullptr, "");
  CPDF_PageContentGenerator generator(other_page.Get());
  const char kExpected[] =
      "q 60 0 0 40 20 60 cm BI /BPC 8 /CS /FXC1 /F /AHx /H 1 /W 1 ID\n"
      "80FF00 >\nEI Q\n";
  {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator, &buf, image);
    EXPECT_EQ(kExpected, ByteString(buf));
  }
  RetainPtr<const CPDF_Dictionary> other_color_spaces =
      other_page->GetResources()->GetDictFor("ColorSpace");
  ASSERT_TRUE(other_color_spaces);
  RetainPtr<const CPDF_Array> other_lab =
      other_color_spaces->GetArrayFor("FXC1");
  ASSERT_TRUE(other_lab);
  EXPECT_EQ("Lab", other_lab->GetByteStringAt(0));

  // Generating again reuses the new resource.
  {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator, &buf, image);
    EXPECT_EQ(kExpected, ByteString(buf));
  }
  EXPECT_EQ(1u, other_color_spaces->size());
}

TEST_F(CPDFPageContentGeneratorTest, ProcessInlineImageWithNewBitmap) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  static constexpr char kContents[] =
      "q 60 0 0 40 20 60 cm BI /W 1 /H 1 /CS /RGB /BPC 8 /F /AHx ID\n"
      "80FF00 >\nEI Q\n";
  RetainPtr<CPDF_Page> page = CreateParsedPage(doc.get(), nullptr, kContents);
  ASSERT_EQ(1u, page->GetPageObjectCount());
  CPDF_ImageObject* image_obj = page->GetPageObjectByIndex(0)->AsImage();
  ASSERT_TRUE(image_obj);
  ASSERT_TRUE(image_obj->GetImage()->IsInline());

  // Replacing the bitmap gives the image the dictionary of an image XObject,
  // possibly with a soft mask. It can only be written as an XObject.
  auto bitmap = pdfium::MakeRetain<CFX_DIBitmap>();
  ASSERT_TRUE(bitmap->Create(2, 2, FXDIB_Format::kBgra));
  image_obj->GetImage()->SetImage(bitmap);
  EXPECT_FALSE(image_obj->GetImage()->IsInline());

  CPDF_PageContentGenerator generator(page.Get());
  fxcrt::ostringstream buf;
  TestProcessPageObject(&generator, &buf, image_obj);
  EXPECT_EQ("q 60 0 0 40 20 60 cm /FXX1 Do Q\n", ByteString(buf));
  RetainPtr<const CPDF_Dictionary> xobjects =
      page->GetResources()->GetDictFor("XObject");
  ASSERT_TRUE(xobjects);
  RetainPtr<const CPDF_Stream> xobject = xobjects->GetStreamFor("FXX1");
  ASSERT_TRUE(xobject);
  EXPECT_EQ(2, xobject->GetDict()->GetIntegerFor("Width"));
}

namespace {

// Makes `shading` an axial shading from red to blue along the x axis.
void FillAxialShading(CPDF_Dictionary* shading) {
  shading->SetNewFor<CPDF_Number>("ShadingType", 2);
  shading->SetNewFor<CPDF_Name>("ColorSpace", "DeviceRGB");
  auto coords = shading->SetNewFor<CPDF_Array>("Coords");
  for (int value : {0, 0, 1, 0}) {
    coords->AppendNew<CPDF_Number>(value);
  }
  auto function = shading->SetNewFor<CPDF_Dictionary>("Function");
  function->SetNewFor<CPDF_Number>("FunctionType", 2);
  auto domain = function->SetNewFor<CPDF_Array>("Domain");
  domain->AppendNew<CPDF_Number>(0);
  domain->AppendNew<CPDF_Number>(1);
  auto c0 = function->SetNewFor<CPDF_Array>("C0");
  for (int value : {1, 0, 0}) {
    c0->AppendNew<CPDF_Number>(value);
  }
  auto c1 = function->SetNewFor<CPDF_Array>("C1");
  for (int value : {0, 0, 1}) {
    c1->AppendNew<CPDF_Number>(value);
  }
  function->SetNewFor<CPDF_Number>("N", 1);
}

}  // namespace

TEST_F(CPDFPageContentGeneratorTest, ProcessShading) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  // One shading that is an indirect object, one that is a direct object of
  // the resources dictionary.
  auto resources = doc->New<CPDF_Dictionary>();
  auto shadings = resources->SetNewFor<CPDF_Dictionary>("Shading");
  auto indirect_shading = doc->NewIndirect<CPDF_Dictionary>();
  FillAxialShading(indirect_shading.Get());
  shadings->SetNewFor<CPDF_Reference>("Sh1", doc.get(),
                                      indirect_shading->GetObjNum());
  FillAxialShading(shadings->SetNewFor<CPDF_Dictionary>("Sh2").Get());

  static constexpr char kContents[] =
      "q 10 20 100 50 re W n 2 0 0 2 30 40 cm /Sh1 sh Q\n"
      "/Sh2 sh\n"
      "q 1 0 0 1 5 6 cm 3 0 0 3 0 0 cm /Sh1 sh Q\n";
  RetainPtr<CPDF_Page> page = CreateParsedPage(doc.get(), resources, kContents);
  ASSERT_EQ(3u, page->GetPageObjectCount());
  for (size_t i = 0; i < 3; ++i) {
    ASSERT_TRUE(page->GetPageObjectByIndex(i)->IsShading());
  }

  CPDF_PageContentGenerator generator(page.Get());
  auto generate = [&](size_t index) {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator, &buf, page->GetPageObjectByIndex(index));
    return ByteString(buf);
  };

  // The clip, the matrix and the resource name are all kept.
  EXPECT_EQ("q 10 20 100 50 re W n 2 0 0 2 30 40 cm /Sh1 sh Q\n", generate(0));
  EXPECT_EQ("q /Sh2 sh Q\n", generate(1));
  EXPECT_EQ("q 3 0 0 3 5 6 cm /Sh1 sh Q\n", generate(2));

  // No resource was added, and the direct shading stays a direct object.
  EXPECT_EQ(2u, shadings->size());
  EXPECT_EQ(0u, shadings->GetDictFor("Sh2")->GetObjNum());

  // Parsing the generated content gives the same objects again.
  ByteString regenerated = generate(0) + generate(1) + generate(2);
  RetainPtr<CPDF_Page> page2 =
      CreateParsedPage(doc.get(), resources, regenerated.AsStringView());
  ASSERT_EQ(3u, page2->GetPageObjectCount());
  CPDF_PageContentGenerator generator2(page2.Get());
  ByteString regenerated2;
  for (size_t i = 0; i < 3; ++i) {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator2, &buf, page2->GetPageObjectByIndex(i));
    regenerated2 += ByteString(buf);
  }
  EXPECT_EQ(regenerated, regenerated2);
}

TEST_F(CPDFPageContentGeneratorTest, ProcessShadingOnAnotherPage) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  auto resources = doc->New<CPDF_Dictionary>();
  auto shadings = resources->SetNewFor<CPDF_Dictionary>("Shading");
  RetainPtr<CPDF_Dictionary> direct_shading =
      shadings->SetNewFor<CPDF_Dictionary>("Sh1");
  FillAxialShading(direct_shading.Get());

  RetainPtr<CPDF_Page> page =
      CreateParsedPage(doc.get(), resources, "50 0 0 50 10 10 cm /Sh1 sh\n");
  ASSERT_EQ(1u, page->GetPageObjectCount());
  CPDF_PageObject* shading_obj = page->GetPageObjectByIndex(0);
  ASSERT_TRUE(shading_obj->IsShading());

  // On a page without that resource, the shading becomes a resource of that
  // page under a new name. A direct object cannot be shared between two
  // resources dictionaries, so it becomes an indirect object.
  RetainPtr<CPDF_Page> other_page = CreateParsedPage(doc.get(), nullptr, "");
  CPDF_PageContentGenerator generator(other_page.Get());
  const char kExpected[] = "q 50 0 0 50 10 10 cm /FXS1 sh Q\n";
  {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator, &buf, shading_obj);
    EXPECT_EQ(kExpected, ByteString(buf));
  }
  RetainPtr<const CPDF_Dictionary> other_shadings =
      other_page->GetResources()->GetDictFor("Shading");
  ASSERT_TRUE(other_shadings);
  EXPECT_EQ(direct_shading, other_shadings->GetDictFor("FXS1"));
  EXPECT_NE(0u, direct_shading->GetObjNum());
  EXPECT_EQ(direct_shading, shadings->GetDictFor("Sh1"));

  // Generating again reuses the new resource.
  {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator, &buf, shading_obj);
    EXPECT_EQ(kExpected, ByteString(buf));
  }
  EXPECT_EQ(1u, other_shadings->size());
}

namespace {

// Adds the graphics state parameter dictionaries the ExtGState tests use to
// `resources`. /GSm, the one with the soft mask, is an indirect object; the
// others are direct objects of the resources dictionary.
void AddExtGStates(CPDF_TestDocument* doc, CPDF_Dictionary* resources) {
  auto ext_gstates = resources->SetNewFor<CPDF_Dictionary>("ExtGState");

  auto group_dict = doc->New<CPDF_Dictionary>();
  group_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  group_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  auto bbox = group_dict->SetNewFor<CPDF_Array>("BBox");
  for (int value : {0, 0, 100, 100}) {
    bbox->AppendNew<CPDF_Number>(value);
  }
  auto group = group_dict->SetNewFor<CPDF_Dictionary>("Group");
  group->SetNewFor<CPDF_Name>("S", "Transparency");
  group->SetNewFor<CPDF_Name>("CS", "DeviceGray");
  static constexpr char kGroupContents[] = "0.5 g 0 0 100 100 re f";
  auto group_stream = doc->NewIndirect<CPDF_Stream>(
      DataVector<uint8_t>(std::begin(kGroupContents),
                          std::end(kGroupContents) - 1),
      std::move(group_dict));

  auto soft_mask_gs = doc->NewIndirect<CPDF_Dictionary>();
  auto soft_mask = soft_mask_gs->SetNewFor<CPDF_Dictionary>("SMask");
  soft_mask->SetNewFor<CPDF_Name>("S", "Luminosity");
  soft_mask->SetNewFor<CPDF_Reference>("G", doc, group_stream->GetObjNum());
  soft_mask_gs->SetNewFor<CPDF_Number>("ca", 0.5f);
  ext_gstates->SetNewFor<CPDF_Reference>("GSm", doc, soft_mask_gs->GetObjNum());

  auto no_soft_mask_gs = ext_gstates->SetNewFor<CPDF_Dictionary>("GSnone");
  no_soft_mask_gs->SetNewFor<CPDF_Name>("SMask", "None");

  auto overprint_gs = ext_gstates->SetNewFor<CPDF_Dictionary>("GSop");
  overprint_gs->SetNewFor<CPDF_Boolean>("OP", true);
  overprint_gs->SetNewFor<CPDF_Boolean>("op", true);
  overprint_gs->SetNewFor<CPDF_Number>("OPM", 1);

  auto knockout_gs = ext_gstates->SetNewFor<CPDF_Dictionary>("GStk");
  knockout_gs->SetNewFor<CPDF_Boolean>("TK", false);
  knockout_gs->SetNewFor<CPDF_Boolean>("SA", true);

  auto line_gs = ext_gstates->SetNewFor<CPDF_Dictionary>("GSln");
  line_gs->SetNewFor<CPDF_Number>("LW", 6);
  line_gs->SetNewFor<CPDF_Number>("LC", 1);
  line_gs->SetNewFor<CPDF_Number>("LJ", 1);
  line_gs->SetNewFor<CPDF_Number>("ML", 2);
  auto dash = line_gs->SetNewFor<CPDF_Array>("D");
  auto dash_array = dash->AppendNew<CPDF_Array>();
  dash_array->AppendNew<CPDF_Number>(4);
  dash_array->AppendNew<CPDF_Number>(2);
  dash->AppendNew<CPDF_Number>(1);
  line_gs->SetNewFor<CPDF_Number>("FL", 0.5f);

  auto multiply_gs = ext_gstates->SetNewFor<CPDF_Dictionary>("GSa50");
  multiply_gs->SetNewFor<CPDF_Number>("ca", 0.5f);
  multiply_gs->SetNewFor<CPDF_Number>("CA", 0.25f);
  multiply_gs->SetNewFor<CPDF_Name>("BM", "Multiply");

  auto screen_gs = ext_gstates->SetNewFor<CPDF_Dictionary>("GSa20");
  screen_gs->SetNewFor<CPDF_Number>("ca", 0.25f);
  screen_gs->SetNewFor<CPDF_Number>("CA", 0.75f);
  screen_gs->SetNewFor<CPDF_Name>("BM", "Screen");
}

std::vector<ByteString> GraphicsResourceNames(const CPDF_PageObject* object) {
  pdfium::span<const ByteString> names = object->GetGraphicsResourceNames();
  return std::vector<ByteString>(names.begin(), names.end());
}

}  // namespace

TEST_F(CPDFPageContentGeneratorTest, ProcessExtGState) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();
  auto resources = doc->New<CPDF_Dictionary>();
  AddExtGStates(doc.get(), resources.Get());

  // In order: a soft mask set up under a matrix; the soft mask turned off
  // again by a second dictionary; overprint; line parameters from a
  // dictionary, inside the scope of another dictionary; the same with
  // operators that override the dictionary; a dictionary that overrides
  // every entry of the one before it.
  static constexpr char kContents[] =
      "q 2 0 0 2 10 20 cm /GSm gs 0 0 1 rg 0 0 50 50 re f Q\n"
      "q /GSm gs /GSnone gs 0 0.6 0 rg 20 20 260 30 re f Q\n"
      "q /GSop gs 1 0 0 0 k 60 80 100 100 re f Q\n"
      "q /GStk gs\n"
      "q /GSln gs 0 0 0.7 RG 20 175 m 60 145 l S Q\n"
      "q /GSln gs 2 w 0 J [] 0 d 1 i 20 130 m 60 100 l S Q\n"
      "Q\n"
      "q /GSa50 gs /GSa20 gs 0 0 30 30 re f Q\n";
  RetainPtr<CPDF_Page> page = CreateParsedPage(doc.get(), resources, kContents);
  ASSERT_EQ(6u, page->GetPageObjectCount());

  // Each object knows the dictionaries that make up its state, in order.
  using testing::ElementsAre;
  EXPECT_THAT(GraphicsResourceNames(page->GetPageObjectByIndex(0)),
              ElementsAre("GSm"));
  EXPECT_THAT(GraphicsResourceNames(page->GetPageObjectByIndex(1)),
              ElementsAre("GSm", "GSnone"));
  EXPECT_THAT(GraphicsResourceNames(page->GetPageObjectByIndex(2)),
              ElementsAre("GSop"));
  EXPECT_THAT(GraphicsResourceNames(page->GetPageObjectByIndex(3)),
              ElementsAre("GStk", "GSln"));
  EXPECT_THAT(GraphicsResourceNames(page->GetPageObjectByIndex(4)),
              ElementsAre("GStk", "GSln"));
  EXPECT_THAT(GraphicsResourceNames(page->GetPageObjectByIndex(5)),
              ElementsAre("GSa20"));

  CPDF_PageContentGenerator generator(page.Get());
  auto generate = [&](size_t index) {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator, &buf, page->GetPageObjectByIndex(index));
    return ByteString(buf);
  };

  // The soft mask dictionary is applied under the matrix it was applied
  // under originally, and the matrix is undone right after.
  EXPECT_EQ(
      "q 2 0 0 2 10 20 cm /GSm gs .5 0 0 .5 -5 -10 cm 0 0 1 rg 0 G "
      "2 0 0 2 10 20 cm 0 0 50 50 re f Q\n",
      generate(0));
  // No soft mask in the end, so no matrix either.
  EXPECT_EQ("q /GSm gs /GSnone gs 0 .6 0 rg 0 G 20 20 260 30 re f Q\n",
            generate(1));
  EXPECT_EQ("q /GSop gs 1 0 0 0 k 0 G 60 80 100 100 re f Q\n", generate(2));
  // The line parameters are written as operators as well. They agree with
  // the dictionary here.
  EXPECT_EQ(
      "q /GStk gs /GSln gs 0 g 0 0 .7 RG 6 w 1 J 1 j 2 M [4 2] 1 d .5 i "
      "20 175 m 60 145 l S Q\n",
      generate(3));
  // And here they do not: the operators come after the dictionary, also
  // the ones that set a parameter back to its initial value.
  EXPECT_EQ(
      "q /GStk gs /GSln gs 0 g 0 G 2 w 0 J 1 j 2 M [] 0 d 1 i "
      "20 130 m 60 100 l S Q\n",
      generate(4));
  EXPECT_EQ("q /GSa20 gs 0 g 0 G 0 0 30 30 re f Q\n", generate(5));

  // Nothing was added to the resources, and the objects kept their names.
  RetainPtr<const CPDF_Dictionary> ext_gstates =
      resources->GetDictFor("ExtGState");
  EXPECT_EQ(7u, ext_gstates->size());
  EXPECT_THAT(GraphicsResourceNames(page->GetPageObjectByIndex(1)),
              ElementsAre("GSm", "GSnone"));

  // Parsing the generated content gives the same objects again.
  ByteString regenerated;
  for (size_t i = 0; i < 6; ++i) {
    regenerated += generate(i);
  }
  RetainPtr<CPDF_Page> page2 =
      CreateParsedPage(doc.get(), resources, regenerated.AsStringView());
  ASSERT_EQ(6u, page2->GetPageObjectCount());
  CPDF_PageContentGenerator generator2(page2.Get());
  ByteString regenerated2;
  for (size_t i = 0; i < 6; ++i) {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator2, &buf, page2->GetPageObjectByIndex(i));
    regenerated2 += ByteString(buf);
  }
  EXPECT_EQ(regenerated, regenerated2);
}

TEST_F(CPDFPageContentGeneratorTest, ProcessExtGStateWithChangedAlpha) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();
  auto resources = doc->New<CPDF_Dictionary>();
  AddExtGStates(doc.get(), resources.Get());

  static constexpr char kContents[] =
      "q /GSop gs 1 0 0 0 k 60 80 100 100 re f Q\n"
      "q /GSa50 gs 0 0 30 30 re f Q\n";
  RetainPtr<CPDF_Page> page = CreateParsedPage(doc.get(), resources, kContents);
  ASSERT_EQ(2u, page->GetPageObjectCount());
  CPDF_PageObject* overprinted = page->GetPageObjectByIndex(0);
  CPDF_PageObject* blended = page->GetPageObjectByIndex(1);

  CPDF_PageContentGenerator generator(page.Get());
  auto generate = [&](CPDF_PageObject* object) {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator, &buf, object);
    return ByteString(buf);
  };
  using testing::ElementsAre;

  // The parsed dictionary says nothing about alpha, so the new alpha takes
  // one more dictionary, after the parsed one.
  overprinted->mutable_general_state().SetFillAlpha(0.25f);
  EXPECT_EQ("q /GSop gs 1 0 0 0 k 0 G /FXE1 gs 60 80 100 100 re f Q\n",
            generate(overprinted));
  EXPECT_THAT(GraphicsResourceNames(overprinted), ElementsAre("GSop", "FXE1"));
  RetainPtr<const CPDF_Dictionary> ext_gstates =
      resources->GetDictFor("ExtGState");
  RetainPtr<const CPDF_Dictionary> alpha_gs = ext_gstates->GetDictFor("FXE1");
  ASSERT_TRUE(alpha_gs);
  EXPECT_EQ(1u, alpha_gs->size());
  EXPECT_EQ(0.25f, alpha_gs->GetFloatFor("ca"));

  // The parsed dictionary sets alpha and blend mode. Undoing the blend
  // mode takes the default graphics dictionary, which sets all of them
  // back, and one more for the alpha values that are to stay.
  blended->mutable_general_state().SetBlendMode("Normal");
  EXPECT_EQ("q /GSa50 gs 0 g 0 G /FXE2 gs /FXE3 gs 0 0 30 30 re f Q\n",
            generate(blended));
  EXPECT_THAT(GraphicsResourceNames(blended),
              ElementsAre("GSa50", "FXE2", "FXE3"));
  RetainPtr<const CPDF_Dictionary> default_gs = ext_gstates->GetDictFor("FXE2");
  ASSERT_TRUE(default_gs);
  EXPECT_EQ(1.0f, default_gs->GetFloatFor("ca"));
  EXPECT_EQ(1.0f, default_gs->GetFloatFor("CA"));
  EXPECT_EQ("Normal", default_gs->GetNameFor("BM"));
  RetainPtr<const CPDF_Dictionary> kept_alpha_gs =
      ext_gstates->GetDictFor("FXE3");
  ASSERT_TRUE(kept_alpha_gs);
  EXPECT_EQ(2u, kept_alpha_gs->size());
  EXPECT_EQ(0.5f, kept_alpha_gs->GetFloatFor("ca"));
  EXPECT_EQ(0.25f, kept_alpha_gs->GetFloatFor("CA"));

  // Back to what the parsed dictionary says: nothing but that dictionary.
  blended->mutable_general_state().SetBlendMode("Multiply");
  EXPECT_EQ("q /GSa50 gs 0 g 0 G 0 0 30 30 re f Q\n", generate(blended));
  EXPECT_THAT(GraphicsResourceNames(blended), ElementsAre("GSa50"));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessExtGStateOnAnotherPage) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();
  auto resources = doc->New<CPDF_Dictionary>();
  AddExtGStates(doc.get(), resources.Get());

  RetainPtr<CPDF_Page> page =
      CreateParsedPage(doc.get(), resources,
                       "q 1 0 0 1 5 5 cm /GSm gs /GSop gs 0 0 30 30 re f Q\n");
  ASSERT_EQ(1u, page->GetPageObjectCount());
  CPDF_PageObject* object = page->GetPageObjectByIndex(0);
  RetainPtr<const CPDF_Dictionary> ext_gstates =
      resources->GetDictFor("ExtGState");
  RetainPtr<const CPDF_Dictionary> soft_mask_gs =
      ext_gstates->GetDictFor("GSm");
  RetainPtr<const CPDF_Dictionary> overprint_gs =
      ext_gstates->GetDictFor("GSop");
  ASSERT_EQ(0u, overprint_gs->GetObjNum());

  // On a page without those resources, the dictionaries become resources
  // of that page under new names. The one that was a direct object becomes
  // an indirect object, so that both pages refer to the same dictionary.
  RetainPtr<CPDF_Page> other_page = CreateParsedPage(doc.get(), nullptr, "");
  CPDF_PageContentGenerator generator(other_page.Get());
  const char kExpected[] =
      "q 1 0 0 1 5 5 cm /FXE1 gs 1 0 0 1 -5 -5 cm /FXE2 gs 0 g 0 G "
      "1 0 0 1 5 5 cm 0 0 30 30 re f Q\n";
  {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator, &buf, object);
    EXPECT_EQ(kExpected, ByteString(buf));
  }
  RetainPtr<const CPDF_Dictionary> other_ext_gstates =
      other_page->GetResources()->GetDictFor("ExtGState");
  ASSERT_TRUE(other_ext_gstates);
  EXPECT_EQ(soft_mask_gs, other_ext_gstates->GetDictFor("FXE1"));
  EXPECT_EQ(overprint_gs, other_ext_gstates->GetDictFor("FXE2"));
  EXPECT_NE(0u, overprint_gs->GetObjNum());
  EXPECT_EQ(overprint_gs, ext_gstates->GetDictFor("GSop"));
  using testing::ElementsAre;
  EXPECT_THAT(GraphicsResourceNames(object), ElementsAre("FXE1", "FXE2"));

  // Generating again reuses the new resources.
  {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator, &buf, object);
    EXPECT_EQ(kExpected, ByteString(buf));
  }
  EXPECT_EQ(2u, other_ext_gstates->size());
}

TEST_F(CPDFPageContentGeneratorTest, ExtGStateOfInactiveObject) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();
  auto resources = doc->New<CPDF_Dictionary>();
  AddExtGStates(doc.get(), resources.Get());

  RetainPtr<CPDF_Page> page =
      CreateParsedPage(doc.get(), resources,
                       "q /GSop gs 0 0 30 30 re f Q\n"
                       "q /GSa50 gs 40 40 30 30 re f Q\n");
  ASSERT_EQ(2u, page->GetPageObjectCount());
  CPDF_PageObject* overprinted = page->GetPageObjectByIndex(0);
  RetainPtr<const CPDF_Dictionary> overprint_gs =
      page->GetResources()->GetDictFor("ExtGState")->GetDictFor("GSop");
  ASSERT_TRUE(overprint_gs);
  using testing::ElementsAre;

  // The dictionaries that no active object uses leave the resources.
  overprinted->SetIsActive(false);
  {
    CPDF_PageContentGenerator generator(page.Get());
    generator.GenerateContent();
  }
  RetainPtr<const CPDF_Dictionary> ext_gstates =
      page->GetResources()->GetDictFor("ExtGState");
  ASSERT_TRUE(ext_gstates);
  EXPECT_THAT(ext_gstates->GetKeys(), ElementsAre("FXE1", "GSa50"));

  // And the one of an object that is active again comes back, under the
  // name it had.
  overprinted->SetIsActive(true);
  {
    CPDF_PageContentGenerator generator(page.Get());
    generator.GenerateContent();
  }
  ext_gstates = page->GetResources()->GetDictFor("ExtGState");
  ASSERT_TRUE(ext_gstates);
  EXPECT_THAT(ext_gstates->GetKeys(), ElementsAre("FXE1", "GSa50", "GSop"));
  EXPECT_EQ(overprint_gs, ext_gstates->GetDictFor("GSop"));
  EXPECT_THAT(GraphicsResourceNames(overprinted), ElementsAre("GSop"));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessExtGStateInForm) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();
  auto resources = doc->New<CPDF_Dictionary>();
  AddExtGStates(doc.get(), resources.Get());

  // A form with resources of its own, a graphics state parameter dictionary
  // among them.
  auto form_dict = doc->New<CPDF_Dictionary>();
  form_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  form_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  auto bbox = form_dict->SetNewFor<CPDF_Array>("BBox");
  for (int value : {0, 0, 100, 100}) {
    bbox->AppendNew<CPDF_Number>(value);
  }
  auto form_resources = form_dict->SetNewFor<CPDF_Dictionary>("Resources");
  auto form_ext_gstates =
      form_resources->SetNewFor<CPDF_Dictionary>("ExtGState");
  auto form_gs = form_ext_gstates->SetNewFor<CPDF_Dictionary>("GSf");
  form_gs->SetNewFor<CPDF_Boolean>("OP", true);
  static constexpr char kFormContents[] =
      "q /GSf gs 0 0 30 30 re f Q 40 40 30 30 re f";
  auto form_stream = doc->NewIndirect<CPDF_Stream>(
      DataVector<uint8_t>(std::begin(kFormContents),
                          std::end(kFormContents) - 1),
      std::move(form_dict));
  auto xobjects = resources->SetNewFor<CPDF_Dictionary>("XObject");
  xobjects->SetNewFor<CPDF_Reference>("Fm1", doc.get(),
                                      form_stream->GetObjNum());

  // The page sets up a soft mask, alpha and a blend mode, then paints the
  // form.
  RetainPtr<CPDF_Page> page = CreateParsedPage(
      doc.get(), resources, "q 2 0 0 2 10 20 cm /GSm gs /GSa50 gs /Fm1 Do Q\n");
  ASSERT_EQ(1u, page->GetPageObjectCount());
  CPDF_FormObject* form_object = page->GetPageObjectByIndex(0)->AsForm();
  ASSERT_TRUE(form_object);
  using testing::ElementsAre;
  using testing::IsEmpty;
  EXPECT_THAT(GraphicsResourceNames(form_object), ElementsAre("GSm", "GSa50"));

  // The objects of the form have the state the page set up. The
  // dictionaries recorded for them are the ones the content stream of the
  // form applies: those of the page are not resources of the form, and they
  // apply by themselves whenever the page paints the form.
  CPDF_Form* form = form_object->form();
  ASSERT_EQ(2u, form->GetPageObjectCount());
  CPDF_PageObject* first = form->GetPageObjectByIndex(0);
  CPDF_PageObject* second = form->GetPageObjectByIndex(1);
  EXPECT_TRUE(first->general_state().GetSoftMask());
  EXPECT_EQ(0.5f, first->general_state().GetFillAlpha());
  EXPECT_THAT(GraphicsResourceNames(first), ElementsAre("GSf"));
  EXPECT_THAT(GraphicsResourceNames(second), IsEmpty());

  // So the content stream of the form is written with the dictionary of the
  // form, and without the dictionaries of the page or a matrix for the soft
  // mask. Alpha and blend mode are written for every object that does not
  // have the initial values, as they were before; here the values are the
  // ones the page set up.
  CPDF_PageContentGenerator form_generator(form);
  fxcrt::ostringstream buf;
  ASSERT_TRUE(form_generator.ProcessPageObjects(&buf));
  EXPECT_EQ(
      "q /GSf gs 0 g 0 G /FXE1 gs 0 0 30 30 re f Q\n"
      "q 0 g 0 G /FXE1 gs 40 40 30 30 re f Q\n",
      ByteString(buf));
  EXPECT_EQ(2u, form_ext_gstates->size());
  RetainPtr<const CPDF_Dictionary> alpha_gs =
      form_ext_gstates->GetDictFor("FXE1");
  ASSERT_TRUE(alpha_gs);
  EXPECT_EQ(3u, alpha_gs->size());
  EXPECT_EQ(0.5f, alpha_gs->GetFloatFor("ca"));
  EXPECT_EQ(0.25f, alpha_gs->GetFloatFor("CA"));
  EXPECT_EQ("Multiply", alpha_gs->GetNameFor("BM"));
  EXPECT_THAT(GraphicsResourceNames(first), ElementsAre("GSf", "FXE1"));
  EXPECT_THAT(GraphicsResourceNames(second), ElementsAre("FXE1"));

  // The page still applies its dictionaries before it paints the form.
  CPDF_PageContentGenerator generator(page.Get());
  fxcrt::ostringstream page_buf;
  TestProcessPageObject(&generator, &page_buf, form_object);
  EXPECT_EQ(
      "q 2 0 0 2 10 20 cm /GSm gs .5 0 0 .5 -5 -10 cm /GSa50 gs 0 g 0 G "
      "2 0 0 2 10 20 cm /FXX1 Do Q\n",
      ByteString(page_buf));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessMiterLimitAndFlatness) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  RetainPtr<CPDF_Page> page =
      CreateParsedPage(doc.get(), nullptr,
                       "q 8 w 1 M 0.25 i 20 20 m 60 70 l 100 20 l S Q\n"
                       "q 8 w 10 M 1 i 20 20 m 60 70 l 100 20 l S Q\n");
  ASSERT_EQ(2u, page->GetPageObjectCount());

  CPDF_PageContentGenerator generator(page.Get());
  auto generate = [&](size_t index) {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator, &buf, page->GetPageObjectByIndex(index));
    return ByteString(buf);
  };
  EXPECT_EQ("q 0 g 0 G 8 w 1 M .25 i 20 20 m 60 70 l 100 20 l S Q\n",
            generate(0));
  // The initial values are not written.
  EXPECT_EQ("q 0 g 0 G 8 w 20 20 m 60 70 l 100 20 l S Q\n", generate(1));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessRenderingIntent) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();
  auto resources = doc->New<CPDF_Dictionary>();
  auto ext_gstates = resources->SetNewFor<CPDF_Dictionary>("ExtGState");
  auto intent_gs = ext_gstates->SetNewFor<CPDF_Dictionary>("GSri");
  intent_gs->SetNewFor<CPDF_Name>("RI", "Perceptual");

  // In order: the operator; the operator after a dictionary that sets the
  // rendering intent; that dictionary alone; the operator with the default
  // rendering intent, and with a name that is not a rendering intent.
  RetainPtr<CPDF_Page> page =
      CreateParsedPage(doc.get(), resources,
                       "q /Saturation ri 0 0 10 10 re f Q\n"
                       "q /GSri gs /AbsoluteColorimetric ri 0 0 10 10 re f Q\n"
                       "q /GSri gs 0 0 10 10 re f Q\n"
                       "q /RelativeColorimetric ri 0 0 10 10 re f Q\n"
                       "q /NoSuchIntent ri 0 0 10 10 re f Q\n");
  ASSERT_EQ(5u, page->GetPageObjectCount());

  CPDF_PageContentGenerator generator(page.Get());
  auto generate = [&](size_t index) {
    fxcrt::ostringstream buf;
    TestProcessPageObject(&generator, &buf, page->GetPageObjectByIndex(index));
    return ByteString(buf);
  };
  EXPECT_EQ("q 0 g 0 G /Saturation ri 0 0 10 10 re f Q\n", generate(0));
  // The operator comes after the dictionary, as it did.
  EXPECT_EQ("q /GSri gs 0 g 0 G /AbsoluteColorimetric ri 0 0 10 10 re f Q\n",
            generate(1));
  // The dictionary says it all.
  EXPECT_EQ("q /GSri gs 0 g 0 G 0 0 10 10 re f Q\n", generate(2));
  // The default rendering intent is not written, under either name.
  EXPECT_EQ("q 0 g 0 G 0 0 10 10 re f Q\n", generate(3));
  EXPECT_EQ("q 0 g 0 G 0 0 10 10 re f Q\n", generate(4));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessEmptyForm) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();
  auto pStream =
      pdfium::MakeRetain<CPDF_Stream>(pdfium::MakeRetain<CPDF_Dictionary>());

  // Create an empty form.
  auto pTestForm = std::make_unique<CPDF_Form>(doc.get(), nullptr, pStream);
  pTestForm->ParseContent();
  ASSERT_EQ(CPDF_PageObjectHolder::ParseState::kParsed,
            pTestForm->GetParseState());

  // The generated stream for the empty form should be an empty string.
  CPDF_PageContentGenerator generator(pTestForm.get());
  fxcrt::ostringstream buf;
  EXPECT_FALSE(generator.ProcessPageObjects(&buf));
  EXPECT_EQ("", ByteString(buf));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessFormWithPath) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();
  static constexpr uint8_t kContents[] =
      "q 3.102 4.6700001 m 5.4500012 .28999999 "
      "l 4.2399998 3.1499999 4.65 2.98 3.456 0.24 c 3.102 4.6700001 l h f Q\n";
  auto pStream = pdfium::MakeRetain<CPDF_Stream>(
      DataVector<uint8_t>(std::begin(kContents), std::end(kContents)),
      pdfium::MakeRetain<CPDF_Dictionary>());

  // Create a form with a non-empty stream.
  auto pTestForm = std::make_unique<CPDF_Form>(doc.get(), nullptr, pStream);
  pTestForm->ParseContent();
  ASSERT_EQ(CPDF_PageObjectHolder::ParseState::kParsed,
            pTestForm->GetParseState());

  CPDF_PageContentGenerator generator(pTestForm.get());
  fxcrt::ostringstream process_buf;
  ASSERT_TRUE(generator.ProcessPageObjects(&process_buf));
  EXPECT_EQ(
      "q 3.102 4.67 m 5.4500012 .29 l 4.24 3.1499999 "
      "4.65 2.98 3.456 .24 c 3.102 4.67 l h f Q\n",
      ByteString(process_buf));
}

TEST_F(CPDFPageContentGeneratorTest, ProcessContentMarksWithProperties) {
  auto doc = std::make_unique<CPDF_TestDocument>();
  doc->CreateNewDoc();

  RetainPtr<CPDF_Dictionary> page_dict(doc->CreateNewPage(0));
  auto test_page = pdfium::MakeRetain<CPDF_Page>(doc.get(), page_dict);

  auto path_obj = std::make_unique<CPDF_PathObject>();
  path_obj->set_filltype(CFX_FillRenderOptions::FillType::kWinding);
  path_obj->path().AppendRect(0, 0, 10, 10);
  path_obj->SetDirty(true);

  auto marks_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  path_obj->GetContentMarks()->AddMarkWithPropertiesHolder(
      "M1", marks_dict, "Property Name With Space");

  test_page->AppendPageObject(std::move(path_obj));

  CPDF_PageContentGenerator generator(test_page.Get());
  fxcrt::ostringstream buf;
  EXPECT_TRUE(generator.ProcessPageObjects(&buf));
  ByteString content(buf);

  EXPECT_TRUE(content.Contains("/M1 /Property#20Name#20With#20Space BDC"));
}
