/**
 * @file
 * @brief Unit tests for AnalogTape effect
 * @author Jonathan Thomas <jonathan@openshot.org>
 */

#include <QColor>
#include <QImage>
#include <memory>

#include "Frame.h"
#include "effects/AnalogTape.h"
#include "openshot_catch.h"

using namespace openshot;

static std::shared_ptr<Frame> makeGrayFrame() {
	QImage img(5, 5, QImage::Format_ARGB32);
	img.fill(QColor(100, 100, 100, 255));
	auto f = std::make_shared<Frame>();
	*f->GetImage() = img;
	return f;
}

static std::shared_ptr<Frame> makeGrayFrame(int w, int h) {
	QImage img(w, h, QImage::Format_ARGB32);
	img.fill(QColor(100, 100, 100, 255));
	auto f = std::make_shared<Frame>();
	*f->GetImage() = img;
	return f;
}

TEST_CASE("AnalogTape modifies frame", "[effect][analogtape]") {
	AnalogTape eff;
	auto frame = makeGrayFrame();
	QColor before = frame->GetImage()->pixelColor(2, 2);
	auto out = eff.GetFrame(frame, 1);
	QColor after = out->GetImage()->pixelColor(2, 2);
	CHECK(after != before);
}

TEST_CASE("AnalogTape deterministic per id", "[effect][analogtape]") {
	AnalogTape e1;
	e1.Id("same");
	AnalogTape e2;
	e2.Id("same");
	auto f1 = makeGrayFrame();
	auto f2 = makeGrayFrame();
	auto o1 = e1.GetFrame(f1, 1);
	auto o2 = e2.GetFrame(f2, 1);
	QColor c1 = o1->GetImage()->pixelColor(1, 1);
	QColor c2 = o2->GetImage()->pixelColor(1, 1);
	CHECK(c1 == c2);
}

TEST_CASE("AnalogTape seed offset alters output", "[effect][analogtape]") {
	AnalogTape e1;
	e1.Id("seed");
	e1.seed_offset = 0;
	AnalogTape e2;
	e2.Id("seed");
	e2.seed_offset = 5;
	auto f1 = makeGrayFrame();
	auto f2 = makeGrayFrame();
	auto o1 = e1.GetFrame(f1, 1);
	auto o2 = e2.GetFrame(f2, 1);
	QColor c1 = o1->GetImage()->pixelColor(1, 1);
	QColor c2 = o2->GetImage()->pixelColor(1, 1);
	CHECK(c1 != c2);
}

TEST_CASE("AnalogTape stripe lifts bottom", "[effect][analogtape]") {
	AnalogTape e;
	e.tracking = Keyframe(0.0);
	e.bleed = Keyframe(0.0);
	e.softness = Keyframe(0.0);
	e.noise = Keyframe(0.0);
	e.stripe = Keyframe(1.0);
	e.staticBands = Keyframe(0.0);
	auto frame = makeGrayFrame(20, 20);
	auto out = e.GetFrame(frame, 1);
	QColor top = out->GetImage()->pixelColor(10, 0);
	QColor bottom = out->GetImage()->pixelColor(10, 19);
	CHECK(bottom.red() > top.red());
}
