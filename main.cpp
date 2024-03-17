/* MADE IN KRASNODAR */ 

#include <iostream>
#include <sstream>
#include <thread>
#include <memory>
#include <queue>
#include <future>
#include <array>
#include <initializer_list>
#include <uuid/uuid.h>
#include <random>
#include <filesystem>

#include "hv/hlog.h"
#include "hv/HttpServer.h"
#include "argparse.hpp"
#include "parallel_hashmap/phmap.h"
#include "thorvg.h"
#include "fpng.h"
#include "simdjson.h"
#include "watcher.hpp"

namespace fs = std::filesystem;
using phmap::flat_hash_map;
using namespace tvg;
using namespace hv;
using namespace argparse;
using namespace simdjson;


/* UTILITY FUNCTIONS */ 

std::string uuidString() {
    uuid_t uuid;
    char uuid_str[37];
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, uuid_str);
    return std::string(uuid_str);
}


std::string padNumber(int number, int length) {
    std::ostringstream oss;
    oss << std::setw(length) << std::setfill('0') << number;
    return oss.str();
}

void flipOrder(std::vector<uint32_t> &buffer) {
    for (uint32_t &pixel : buffer) {
        uint8_t *p = reinterpret_cast<uint8_t*>(&pixel);
        uint8_t reverse[4] = {p[3], p[2], p[1], p[0]};
        pixel = *reinterpret_cast<uint32_t*>(reverse);
    }
}

/* CANVAS SETTINGS */

struct Color {
    uint8_t r, g, b, a;
};

struct Range {
    float min, max;
};

struct Point2 {
    float x, y;
};

struct Bounds {
    Point2 origin;  
    float width, height;
};


class JsonDocument {
    public:
    dom::element dom;

    JsonDocument(std::string filename) {
        dom = parser->load(filename);
    }

    JsonDocument(padded_string& json) {
        dom = parser->parse(json);
    }

    void reload(std::string filename) {
        std::lock_guard<std::mutex> lock(mutex);
        try {
            auto newDoc = JsonDocument(filename);
            dom = newDoc.dom;
            parser.reset(newDoc.parser.release());
        } catch (std::exception &ex) {
            std::cout << "failed to load " << filename << std::endl << std::flush;
        }  
    }

    private:
    std::mutex mutex;
    std::unique_ptr<dom::parser> parser = std::make_unique<dom::parser>();
};



class SettingsException : public std::exception {
    public:
    SettingsException(std::string details) {
        std::stringstream msg;
        msg << "Settings exception : " << details << std::endl;
        message = msg.str();
    }

    const char* what() const noexcept override {
        return message.c_str();
    }

    private: 
    std::string message;
};



class Settings {
    public:

    Settings(std::string filename) : 
        defaults(defaultJson),
        supplied(filename),
        watcher(fs::path(filename).parent_path(), [this, filename](wtr::event ev) {
            if (ev.path_name.filename() == fs::path(filename).filename() && fs::file_size(filename) > 0) {
                supplied.reload(filename);
                std::cout << "settings reloaded " << ev.path_name << std::endl << std::flush;
            }
        }) {}

    float getFloat(std::string path) {
        double value;
        getValue<double>(path, value);
        return static_cast<float>(value);
    }

    Range getRange(std::string path) {
        dom::array value;
        getValue<dom::array>(path, value);
        if (value.size() >= 2) {
            float min = static_cast<float>(value.at(0).get_double());
            float max = static_cast<float>(value.at(1).get_double());
            return {min, max};
        } else {
            throw SettingsException("can't parse range from " + path);
        }
    }

    Color getColor(std::string path) {
        dom::array value;
        getValue<dom::array>(path, value);
        if (value.size() >= 4) {
            uint8_t r = static_cast<uint8_t>(value.at(0).get_uint64());
            uint8_t g = static_cast<uint8_t>(value.at(1).get_uint64());
            uint8_t b = static_cast<uint8_t>(value.at(2).get_uint64());
            uint8_t a = static_cast<uint8_t>(value.at(3).get_uint64());    
            return {r, g, b, a};
        } else {
            throw SettingsException("can't parse color from " + path);
        }
    }

    private:

    template <typename T> 
    void getValue(std::string pointer, T& value) {
        auto status = supplied.dom.at_pointer(pointer).get(value);
        if (status != SUCCESS) {
            status = defaults.dom.at_pointer(pointer).get(value);
            if (status != SUCCESS) {
                throw SettingsException("json error at " + pointer);
            }
        }
    }

    padded_string defaultJson = R"(
    {    
        "height" : 500,
        "width"  : 500,
        "background" : [200, 200, 200, 255],
        "strokeWidth" : 2,
        "strokeColor" : [0, 0, 0, 255],
        "fillColor" : [0, 0, 0, 0],
        "countRange" : [3, 13],
        "arguments" : {
            "randomShapes" : {
                "sizeRange" : [50, 150],
                "angleRange" : [0, 360]
            }
        }
    }
    )"_padded;

    JsonDocument supplied;
    JsonDocument defaults;

    wtr::watcher::watch watcher;
};


/* DRAWING FUNCTIONS */

using CanvasPtr = std::unique_ptr<tvg::SwCanvas>;


auto uniform_number_distribution(float min, float max) {
    return std::uniform_real_distribution<float>(min, max);
}

auto uniform_number_distribution(int min, int max) {
    return std::uniform_int_distribution<int>(min, max);
}

template<typename T>
T randomNumber(T min, T max) {
    std::random_device rd;
    std::mt19937 gen(rd());
    return uniform_number_distribution(min, max)(gen);
}

void randomPoints(std::vector<Point2> &result, Bounds bounds, size_t count) {
    for (int n = 0; n < count; n++) {
        Point2 min = bounds.origin;
        Point2 max = {x : min.x + bounds.width, y: min.y + bounds.height};
        result.push_back({x: randomNumber(min.x, max.x), y: randomNumber(min.y, max.y)});
    }
}

CanvasPtr randomShapes(CanvasPtr canvas, Bounds bounds, Settings &settings, size_t count) {
    std::vector<Point2> centers;
    randomPoints(centers, bounds, count);
    
    auto size  = settings.getRange("/arguments/randomShapes/sizeRange");
    auto angle = settings.getRange("/arguments/randomShapes/angleRange");
    auto strokeColor = settings.getColor("/strokeColor");
    auto strokeWidth = settings.getFloat("/strokeWidth");
    auto fillColor = settings.getColor("/fillColor");

    for (int n = 0; n < count; n++) {
        float s = randomNumber(size.min, size.max);
        float a = randomNumber(angle.min, angle.max);
        Bounds rect = {{s / -2, s / -2}, s, s};

        auto shape = tvg::Shape::gen();
        shape->appendRect(rect.origin.x, rect.origin.y, rect.width, rect.height);
        shape->rotate(a);
        shape->translate(centers[n].x, centers[n].y);
        shape->strokeWidth(strokeWidth);
        shape->strokeFill(strokeColor.r, strokeColor.g, strokeColor.b, strokeColor.a);
        shape->fill(fillColor.r, fillColor.g, fillColor.b, fillColor.a);
        canvas->push(move(shape));
    }
    return canvas;
}


/* SERVER IMPLEMENTATION */


class AppState {
    public:
    Settings settings;
    unsigned int maxKeys = 5000;
    unsigned int shrinkSize = 1000;

    AppState(std::string configFile) : settings(configFile)  {}

    std::string makeKey() {
        auto guid = uuidString();
        auto range = settings.getRange("/countRange");

        std::lock_guard<std::mutex> lock(mutex);
        queue.push(guid);
        keys[guid] = std::to_string(randomNumber(static_cast<int>(range.min), static_cast<int>(range.max)));
        
        if (queue.size() > maxKeys) {
            std::thread([this] () {
                while(shrink()) {}
            }).detach();
        }
        return guid;
    }

    std::string getKey(std::string guid) {
        std::lock_guard<std::mutex> lock(mutex);
        auto index = keys.find(guid);
        if (index != keys.end()) {
            return keys[guid];
        } else {
            throw std::exception();
        }
    }

    void removeKey(std::string guid) {
        std::lock_guard<std::mutex> lock(mutex);
        keys.erase(guid);
    }

    bool shrink() {
        std::lock_guard<std::mutex> lock(mutex);
        for (int n = 0; n < shrinkSize; n++) {
            auto guid = queue.front();
            keys.erase(guid);
            queue.pop();
            if (queue.size() < maxKeys) {
                return false;
            }
        }
        return true;
    }

    private:
    flat_hash_map<std::string, std::string> keys;
    std::queue<std::string> queue;
    std::mutex mutex;
};


int makeCaptcha(AppState& app, const HttpContextPtr& ctx) {
    return ctx->send(app.makeKey());
}


int takeCaptcha(AppState& app, const HttpContextPtr& ctx) {
    auto guid = ctx->param("guid");

    try {
        auto key  = app.getKey(guid);
        
        auto width  = app.settings.getFloat("/width");
        auto height = app.settings.getFloat("/height");
        auto bg     = app.settings.getColor("/background");

        size_t numPixels = width * height;
        std::vector<uint32_t> buffer(numPixels);

        auto canvas = tvg::SwCanvas::gen();
        canvas->target(buffer.data(), width, width, height, tvg::SwCanvas::ABGR8888);

        auto backdrop = tvg::Shape::gen();
        backdrop->appendRect(0, 0, width, height);
        backdrop->fill(bg.r, bg.g, bg.b, bg.a);
        canvas->push(move(backdrop));

        canvas = randomShapes(move(canvas), {{0, 0}, width, height}, app.settings, std::stoi(key));
        canvas->draw();
        canvas->sync();

        std::vector<uint8_t> pngImage;
        fpng::fpng_encode_image_to_memory(buffer.data(), width, height, 4, pngImage);

        ctx->response->SetHeader("Content-Type", "image/png");
        return ctx->sendData(pngImage.data(), numPixels, false);
    } catch (std::exception &ex) {
        return 404;
    }
}


int testCaptcha(AppState& app, const HttpContextPtr& ctx) {
    auto guid = ctx->param("guid");
    auto key = ctx->param("key");
    try {
        auto realKey = app.getKey(guid);
        if (key == realKey) {
            return 200;
        } else {
            app.removeKey(guid);
            return 401;
        }
    } catch (std::exception &ex) {
        return 401;
    }
}


int main(int argc, char** argv) {
    ArgumentParser args("dz-captcha", BUILD_VERSION);
    args.add_argument("-P", "--port")
        .help("port number") 
        .default_value<unsigned int>(80)
        .scan<'u', unsigned int>()
        .nargs(1);
    
    args.add_argument("-W", "--workers")
        .help("number of workers")
        .default_value<unsigned int>(std::thread::hardware_concurrency())
        .scan<'u', unsigned int>()
        .nargs(1);
    
    args.add_argument("-X", "--maxkeys")
        .help("maximum allowed number of keys before cleanup - some kind of DoS protection")
        .default_value<unsigned int>(10000)
        .scan<'u', unsigned int>()
        .nargs(1);
    
    args.add_argument("-S", "--shrinksize")
        .help("total amount of old keys removed on each cleanup step")
        .default_value<unsigned int>(1000)
        .scan<'u', unsigned int>()
        .nargs(1);

    args.add_argument("-C", "--canvas")
        .help("canvas configuration file")
        .default_value<std::string>("./canvas.json")
        .nargs(1);
    
    try {
        args.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << args;
        return 1; 
    }

    AppState app(args.get<std::string>("--canvas"));    

    std::srand(std::time(nullptr));
    hlog_set_handler(stderr_logger);

    HttpService router;

    app.maxKeys = args.get<unsigned int>("--maxkeys");
    app.shrinkSize = args.get<unsigned int>("--shrinksize");

    router.GET("/make", [&app] (const HttpContextPtr& ctx) {
        return makeCaptcha(app, ctx);
    });

    router.GET("/take/{guid}", [&app] (const HttpContextPtr& ctx) {
        return takeCaptcha(app, ctx);
    });

    router.GET("/test/{guid}/{key}", [&app] (const HttpContextPtr& ctx) {
        return testCaptcha(app, ctx);
    });

    HttpServer server(&router);

    server.setPort(args.get<unsigned int>("--port"));
    server.setThreadNum(args.get<unsigned int>("--workers"));

    tvg::Initializer::init(1);
    std::cout << "welcome to dz-captcha microservice" << std::endl;
    server.run();
    return 0;
}