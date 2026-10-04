/* SPDX-License-Identifier: MIT
 * Offline head-region tracker. The user chooses the head once in Kdenlive;
 * OpenCV CSRT writes deterministic coordinates for the renderer.
 */
#include <opencv2/imgproc.hpp>
#include <opencv2/tracking.hpp>
#include <opencv2/videoio.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

struct Sample { double time, x, y, quality; };

static double number(const char *text)
{
    size_t used = 0;
    double value = std::stod(text, &used);
    if (!std::isfinite(value) || text[used] != 0) throw std::runtime_error("invalid number");
    return value;
}

int main(int argc, char **argv)
{
    try {
        if (argc != 9) throw std::runtime_error("usage: studio-head-tracker INPUT OUTPUT START DURATION X Y W H");
        const std::filesystem::path input = argv[1], output = argv[2];
        const double start = number(argv[3]), duration = number(argv[4]);
        const double sx = number(argv[5]), sy = number(argv[6]), sw = number(argv[7]), sh = number(argv[8]);
        if (start < 0 || duration < .15 || duration > 3600 || sx < 0 || sy < 0 || sw < .015 || sh < .015
            || sx + sw > 1.001 || sy + sh > 1.001) throw std::runtime_error("invalid segment or head rectangle");

        cv::setNumThreads(2);
        cv::VideoCapture video(input.string());
        if (!video.isOpened()) throw std::runtime_error("cannot open video");
        const double fps = video.get(cv::CAP_PROP_FPS);
        const int sourceWidth = int(video.get(cv::CAP_PROP_FRAME_WIDTH));
        const int sourceHeight = int(video.get(cv::CAP_PROP_FRAME_HEIGHT));
        const int total = int(video.get(cv::CAP_PROP_FRAME_COUNT));
        if (!std::isfinite(fps) || fps < 1 || fps > 240 || sourceWidth < 2 || sourceHeight < 2 || total < 2)
            throw std::runtime_error("invalid video metadata");
        const int first = std::clamp(int(std::llround(start * fps)), 0, total - 2);
        const int last = std::clamp(int(std::llround((start + duration) * fps)), first + 2, total);
        video.set(cv::CAP_PROP_POS_FRAMES, first);

        cv::Mat frame;
        if (!video.read(frame)) throw std::runtime_error("cannot read the first frame");
        const double scale = std::min(1.0, 960.0 / std::max(frame.cols, frame.rows));
        if (scale < 1) cv::resize(frame, frame, {}, scale, scale, cv::INTER_AREA);
        cv::Rect box(int(std::lround(sx * frame.cols)), int(std::lround(sy * frame.rows)),
                     int(std::lround(sw * frame.cols)), int(std::lround(sh * frame.rows)));
        box &= cv::Rect(0, 0, frame.cols, frame.rows);
        if (box.width < 8 || box.height < 8) throw std::runtime_error("head rectangle is too small");
        auto tracker = cv::TrackerCSRT::create();
        tracker->init(frame, box);
        std::vector<Sample> samples;
        const int sampleStep = std::max(1, int(std::ceil(fps / 30.0)));
        cv::Rect lastBox = box;
        for (int sourceFrame = first; sourceFrame < last; ++sourceFrame) {
            bool found = true;
            if (sourceFrame != first) {
                if (!video.read(frame)) break;
                if (scale < 1) cv::resize(frame, frame, {}, scale, scale, cv::INTER_AREA);
                cv::Rect next = lastBox;
                found = tracker->update(frame, next);
                if (found && next.width >= 4 && next.height >= 4) lastBox = next;
            }
            if ((sourceFrame - first) % sampleStep == 0) {
                samples.push_back({(sourceFrame - first) / fps,
                                   (lastBox.x + lastBox.width * .5) / frame.cols,
                                   (lastBox.y + lastBox.height * .5) / frame.rows,
                                   found ? 1.0 : 0.0});
            }
            if ((sourceFrame - first) % std::max(1, int(fps)) == 0)
                std::cout << "PROGRESS " << (100 * (sourceFrame - first) / std::max(1, last - first)) << '\n' << std::flush;
        }
        if (samples.size() < 2) throw std::runtime_error("not enough tracking samples");

        const int radius = std::min<int>(samples.size() - 1, std::max(1, int(std::ceil(.13 * fps / sampleStep))));
        std::vector<Sample> smooth = samples;
        for (size_t i = 0; i < samples.size(); ++i) {
            double x = 0, y = 0, weight = 0;
            const int from = std::max<int>(0, int(i) - radius), to = std::min<int>(samples.size() - 1, int(i) + radius);
            for (int j = from; j <= to; ++j) {
                const double w = std::exp(-.5 * std::pow((j - int(i)) / std::max(1.0, radius / 2.0), 2));
                x += samples[j].x * w; y += samples[j].y * w; weight += w;
            }
            smooth[i].x = std::clamp(x / weight, 0.0, 1.0);
            smooth[i].y = std::clamp(y / weight, 0.0, 1.0);
        }

        if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
        auto temporary = output;
        temporary += ".tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("cannot create track file");
        stream.imbue(std::locale::classic());
        stream << "SUNIMO_CAMERA_TRACK_V1\n# aspect=" << std::setprecision(12) << double(sourceWidth) / sourceHeight
               << "\n# tracker=OpenCV CSRT; source=" << input.filename().string() << '\n' << std::fixed;
        for (const auto &sample : smooth)
            stream << std::setprecision(9) << sample.time << ',' << sample.x << ',' << sample.y << ','
                   << std::setprecision(6) << sample.quality << '\n';
        stream.close();
        if (!stream) throw std::runtime_error("cannot finish track file");
        std::error_code error;
        std::filesystem::rename(temporary, output, error);
        if (error) throw std::runtime_error("cannot publish track file");
        std::cout << "PROGRESS 100\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Studio head tracker: " << error.what() << '\n';
        return 1;
    }
}
