// shop_check: the print failure check's comparison as a command-line tool, for the always-on checker
// service (shop_checkd.py), which runs it once per checkpoint. Same engine as the slicer
// (src/slic3r/GUI/FailureCheck.*), so a job checked on the server gets the slicer's verdicts.
//
//   shop_check info <job.gcode>
//       {"checkpoints": [{"index", "kind", "label"}...]}
//   shop_check compare <job.gcode> <camera.json> <index> <base.jpg> <base_z> <cur.jpg> <z> [alignment.json]
//       {"verdict": "pass"|"fail"|"unsure", "unexpected_mm2", "missing_fraction", "reasons": [...],
//        "first_layer": bool, "alignment": [[x, y, dx, dy, s]...]}
//       or {"review_reason": "..."} when the photos cannot be compared.
//
// Prints one JSON object on stdout; exit status 0 unless the arguments or the G-code are unusable.
#include "slic3r/GUI/FailureCheck.hpp"

#include <nlohmann/json.hpp>
#include <turbojpeg.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

using namespace Slic3r::GUI::FailureCheck;
using nlohmann::json;

static bool read_file(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

static bool decode_jpeg(const std::string& path, Image& img)
{
    std::string bytes;
    if (!read_file(path, bytes) || bytes.empty())
        return false;
    tjhandle tj = tjInitDecompress();
    if (!tj)
        return false;
    int  w = 0, h = 0, sub = 0, cs = 0;
    bool ok = tjDecompressHeader3(tj, reinterpret_cast<const unsigned char*>(bytes.data()), (unsigned long) bytes.size(), &w, &h, &sub, &cs) == 0 &&
              w > 0 && h > 0 && w <= 8192 && h <= 8192;
    if (ok) {
        img.w = w;
        img.h = h;
        img.rgb.assign(size_t(w) * h * 3, 0);
        ok = tjDecompress2(tj, reinterpret_cast<const unsigned char*>(bytes.data()), (unsigned long) bytes.size(), img.rgb.data(), w, 0, h,
                           TJPF_RGB, 0) == 0;
    }
    tjDestroy(tj);
    return ok;
}

static const char* kind_name(CheckpointKind k)
{
    switch (k) {
    case CheckpointKind::Baseline: return "baseline";
    case CheckpointKind::Early: return "early";
    case CheckpointKind::FirstLayer: return "first_layer";
    case CheckpointKind::EarlyLayers: return "early_layers";
    case CheckpointKind::Progress: return "progress";
    }
    return "unknown";
}

static int fail(const std::string& msg)
{
    std::printf("%s\n", json{{"error", msg}}.dump().c_str());
    return 2;
}

int main(int argc, char** argv)
{
    if (argc < 3)
        return fail("usage: shop_check info <gcode> | compare <gcode> <camera.json> <index> <base.jpg> <base_z> <cur.jpg> <z> [alignment.json]");
    const std::string cmd = argv[1];
    GCodeModel        model;
    std::string       err;
    if (!parse_gcode(argv[2], model, err))
        return fail("cannot read the G-code: " + err);

    if (cmd == "info") {
        json cps = json::array();
        for (const Checkpoint& cp : model.checkpoints)
            cps.push_back({{"index", cp.index}, {"kind", kind_name(cp.kind)}, {"label", checkpoint_label(cp)}});
        std::printf("%s\n", json{{"checkpoints", cps}}.dump().c_str());
        return 0;
    }
    if (cmd != "compare" || argc < 9)
        return fail("unknown command or missing arguments");

    const int         index = std::atoi(argv[4]);
    const Checkpoint* cp    = model.checkpoint(index);
    if (cp == nullptr)
        return fail("the G-code has no checkpoint " + std::to_string(index));
    const bool first_layer = cp->kind == CheckpointKind::Early || cp->kind == CheckpointKind::FirstLayer;

    std::string cam_text;
    CameraModel cam;
    if (!read_file(argv[3], cam_text) || !CameraModel::from_json(cam_text, cam)) {
        std::printf("%s\n", json{{"review_reason", "the camera is not calibrated yet"}}.dump().c_str());
        return 0;
    }
    Image base, cur;
    if (!decode_jpeg(argv[7], cur) || cur.w != cam.width || cur.h != cam.height) {
        std::printf("%s\n", json{{"review_reason", "the camera photo does not match the calibration's size"}}.dump().c_str());
        return 0;
    }
    if (!decode_jpeg(argv[5], base) || base.w != cur.w || base.h != cur.h) {
        std::printf("%s\n", json{{"review_reason", "this job has no reference photo of the empty plate"}}.dump().c_str());
        return 0;
    }
    Alignment prior;
    if (!first_layer && argc > 9) {
        std::string text;
        if (read_file(argv[9], text)) {
            const json a = json::parse(text, nullptr, false);
            if (a.is_array())
                for (const json& an : a)
                    if (an.is_array() && an.size() >= 4)
                        prior.anchors.push_back({an[0].get<double>(), an[1].get<double>(), an[2].get<double>(), an[3].get<double>(),
                                                 an.size() >= 5 ? an[4].get<double>() : 1.0});
        }
    }
    const CheckResult res = compare(model, *cp, cam, base, std::atof(argv[6]), cur, std::atof(argv[8]), prior);
    json              anchors = json::array();
    for (const auto& an : res.alignment.anchors)
        anchors.push_back({an.x, an.y, an.dx, an.dy, an.s});
    const char* verdict = res.verdict == Verdict::Pass ? "pass" : res.verdict == Verdict::Fail ? "fail" : "unsure";
    std::printf("%s\n", json{{"verdict", verdict},
                             {"label", checkpoint_label(*cp)},
                             {"unexpected_mm2", res.unexpected_mm2},
                             {"missing_fraction", res.missing_fraction},
                             {"reasons", res.reasons},
                             {"first_layer", first_layer},
                             {"alignment", anchors}}
                            .dump()
                            .c_str());
    return 0;
}
