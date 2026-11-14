// ONVIF PTZ over SOAP 1.2 with WS-Security UsernameToken digest.
#include "internal.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace ptz {

namespace {

constexpr const char *kDeviceNs = "http://www.onvif.org/ver10/device/wsdl";
constexpr const char *kMediaNs = "http://www.onvif.org/ver10/media/wsdl";
constexpr const char *kPtzNs = "http://www.onvif.org/ver20/ptz/wsdl";
constexpr const char *kSchemaNs = "http://www.onvif.org/ver10/schema";
// ONVIF PTZ spec, Annex A.3.2: origin at the picture's centre, (1,1) at its upper right corner.
constexpr const char *kFovSpace = "http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationSpaceFov";

struct Space {
    std::string uri;
    Range x, y;
};

Range range_of(const XmlNode *node) {
    Range range;
    if (!node) return range;
    auto low = parse_number(node->child_text("Min"));
    auto high = parse_number(node->child_text("Max"));
    if (low && high && *low <= *high) range = {true, *low, *high};
    return range;
}

std::vector<Space> spaces_of(const XmlNode &spaces, std::string_view name) {
    std::vector<Space> result;
    for (auto node : spaces.children_named(name))
        result.push_back({node->child_text("URI"), range_of(node->child("XRange")), range_of(node->child("YRange"))});
    return result;
}

Space choose_space(const std::vector<Space> &spaces, const std::string &default_uri, const char *label) {
    if (spaces.empty()) throw Error(std::string("Camera does not advertise an absolute ") + label + " space");
    if (!default_uri.empty()) {
        for (const auto &space : spaces)
            if (space.uri == default_uri) return space;
        throw Error(std::string("Camera's default absolute ") + label + " space is not advertised");
    }
    return spaces.front();
}

void check_range(double value, const Range &range, const char *label) {
    if (!range.available) throw Error(std::string(label) + " range is unavailable; move not sent");
    if (value < range.min || value > range.max)
        throw Error(std::string(label) + " " + format_number(value) + " is outside the camera's range [" +
                    format_number(range.min) + ", " + format_number(range.max) + "]");
}

std::string iso_utc(std::chrono::system_clock::time_point when) {
    std::time_t t = std::chrono::system_clock::to_time_t(when);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

Reading reading_of(const std::string &text) {
    Reading reading;
    if (auto value = parse_number(text)) reading = {true, *value, trim(text)};
    return reading;
}

class Onvif final : public Camera {
public:
    Onvif(CameraParams params, const Hooks &hooks) : p_(std::move(params)), hooks_(hooks) {
        base_ = "http://" + bracket_host(p_.host) + ":" + std::to_string(p_.port);
        sync_clock();
        discover_services();

        auto profiles = call(media_, kMediaNs, "http://www.onvif.org/ver10/media/wsdl/GetProfiles",
                             "<trt:GetProfiles/>", "trt");
        auto list = profiles.children_named("Profiles");
        if (p_.profile >= static_cast<int>(list.size()))
            throw Error("Profile " + std::to_string(p_.profile) + " does not exist; camera returned " +
                        std::to_string(list.size()) + " profiles");
        const XmlNode &profile = *list[p_.profile];
        profile_token_ = profile.attribute("token");
        const XmlNode *config = profile.child("PTZConfiguration");
        if (!config) throw Error("Selected profile has no PTZ configuration; try another profile index");
        std::string config_token = config->attribute("token");
        // "Pant" is the ONVIF schema's own spelling.
        std::string default_pan_tilt = config->child_text("DefaultAbsolutePantTiltPositionSpace");
        std::string default_zoom = config->child_text("DefaultAbsoluteZoomPositionSpace");

        auto options = call(ptz_, kPtzNs, "http://www.onvif.org/ver20/ptz/wsdl/GetConfigurationOptions",
                            "<tptz:GetConfigurationOptions><tptz:ConfigurationToken>" + xml_escape(config_token) +
                                "</tptz:ConfigurationToken></tptz:GetConfigurationOptions>",
                            "tptz");
        const XmlNode *spaces = options.find("Spaces");
        if (!spaces) throw Error("Camera did not return PTZ coordinate spaces");
        pan_tilt_ = choose_space(spaces_of(*spaces, "AbsolutePanTiltPositionSpace"), default_pan_tilt, "pan/tilt");
        pan_range = pan_tilt_.x;
        tilt_range = pan_tilt_.y;
        for (const auto &space : spaces_of(*spaces, "RelativePanTiltTranslationSpace"))
            if (space.uri == kFovSpace) fov_ = space;
        auto zooms = spaces_of(*spaces, "AbsoluteZoomPositionSpace");
        if (!zooms.empty()) {
            zoom_ = choose_space(zooms, default_zoom, "zoom");
            zoom_range = zoom_->x;
        }
    }

    std::string name() const override { return "Bosch/Milesight · ONVIF"; }
    std::string units() const override {
        return "ONVIF absolute coordinates (commonly pan/tilt −1…1, zoom 0…1; not degrees)";
    }

    Position position() override {
        auto body = call(ptz_, kPtzNs, "http://www.onvif.org/ver20/ptz/wsdl/GetStatus",
                         "<tptz:GetStatus><tptz:ProfileToken>" + xml_escape(profile_token_) +
                             "</tptz:ProfileToken></tptz:GetStatus>",
                         "tptz");
        Position result;
        const XmlNode *position = body.find("Position");
        if (const XmlNode *pt = position ? position->child("PanTilt") : nullptr) {
            std::string space = pt->attribute("space");
            if (!space.empty() && space != pan_tilt_.uri)
                throw Error("Camera reported pan/tilt in a different coordinate space");
            result.pan = reading_of(pt->attribute("x"));
            result.tilt = reading_of(pt->attribute("y"));
        }
        if (const XmlNode *zoom = position ? position->child("Zoom") : nullptr; zoom && zoom_) {
            std::string space = zoom->attribute("space");
            if (!space.empty() && space != zoom_->uri)
                throw Error("Camera reported zoom in a different coordinate space");
            result.zoom = reading_of(zoom->attribute("x"));
        }
        if (const XmlNode *status = body.find("MoveStatus")) {
            std::string pt = status->child_text("PanTilt"), z = status->child_text("Zoom");
            if (pt == "MOVING" || z == "MOVING") result.move_status = "MOVING";
            else if (!pt.empty() || !z.empty()) result.move_status = pt.empty() ? z : pt;
        }
        if (!result.pan.available && !result.tilt.available && !result.zoom.available)
            throw Error("Camera did not report its current coordinates");
        return result;
    }

    void move(const Target &target) override {
        auto pan = target.pan, tilt = target.tilt;
        std::string position;
        if (pan || tilt) {
            if (!pan || !tilt) {
                // AbsoluteMove needs both pan and tilt; keep the unchanged axis where it is.
                Position now;
                try {
                    now = this->position();
                } catch (const std::exception &e) {
                    throw Error(std::string("Cannot preserve the unchanged pan/tilt axis: ") + e.what());
                }
                if (!pan && !now.pan.available) throw Error("Cannot preserve pan: camera did not report it");
                if (!tilt && !now.tilt.available) throw Error("Cannot preserve tilt: camera did not report it");
                if (!pan) pan = now.pan.value;
                if (!tilt) tilt = now.tilt.value;
            }
            check_range(*pan, pan_range, "Pan");
            check_range(*tilt, tilt_range, "Tilt");
            position += "<tt:PanTilt x=\"" + format_number(*pan) + "\" y=\"" + format_number(*tilt) +
                        "\" space=\"" + xml_escape(pan_tilt_.uri) + "\"/>";
        }
        if (target.zoom) {
            if (!zoom_) throw Error("Camera does not advertise an absolute zoom space");
            check_range(*target.zoom, zoom_range, "Zoom");
            position += "<tt:Zoom x=\"" + format_number(*target.zoom) + "\" space=\"" + xml_escape(zoom_->uri) + "\"/>";
        }
        if (position.empty()) throw Error("All axes unchanged; no move sent");
        call(ptz_, kPtzNs, "http://www.onvif.org/ver20/ptz/wsdl/AbsoluteMove",
             "<tptz:AbsoluteMove><tptz:ProfileToken>" + xml_escape(profile_token_) +
                 "</tptz:ProfileToken><tptz:Position>" + position + "</tptz:Position></tptz:AbsoluteMove>",
             "tptz");
    }

    void stop() override {
        call(ptz_, kPtzNs, "http://www.onvif.org/ver20/ptz/wsdl/Stop",
             "<tptz:Stop><tptz:ProfileToken>" + xml_escape(profile_token_) +
                 "</tptz:ProfileToken><tptz:PanTilt>true</tptz:PanTilt><tptz:Zoom>true</tptz:Zoom></tptz:Stop>",
             "tptz");
    }

    std::string picture() override {
        if (snapshot_uri_.empty()) {
            auto body = call(media_, kMediaNs, "http://www.onvif.org/ver10/media/wsdl/GetSnapshotUri",
                             "<trt:GetSnapshotUri><trt:ProfileToken>" + xml_escape(profile_token_) +
                                 "</trt:ProfileToken></trt:GetSnapshotUri>",
                             "trt");
            const XmlNode *uri = body.find("Uri");
            if (!uri || trim(uri->text).empty()) throw Error("Camera did not give an address for its picture");
            snapshot_uri_ = route(trim(uri->text));
        }
        return fetch_picture(snapshot_uri_, p_.user, p_.password, p_.timeout);
    }

    std::string stream_uri() override {
        auto body = call(media_, kMediaNs, "http://www.onvif.org/ver10/media/wsdl/GetStreamUri",
                         "<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream><tt:Transport>"
                         "<tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup><trt:ProfileToken>" +
                             xml_escape(profile_token_) + "</trt:ProfileToken></trt:GetStreamUri>",
                         "trt");
        const XmlNode *uri = body.find("Uri");
        if (!uri || trim(uri->text).empty()) throw Error("Camera did not give an address for its video");
        std::string text = trim(uri->text);
        if (lower(text).rfind("rtsp://", 0) != 0) throw Error("Camera gave a video address that isn't RTSP");
        return text;
    }

    bool view_moves() const override { return fov_.has_value(); }

    void move_in_view(double x, double y, int, int) override {
        if (!fov_) throw Error("This camera can't aim from its picture: it doesn't offer ONVIF moves within the view");
        auto clamp = [](double v, const Range &r) { return r.available ? std::min(std::max(v, r.min), r.max) : v; };
        x = clamp(x, fov_->x);
        y = clamp(y, fov_->y);
        call(ptz_, kPtzNs, "http://www.onvif.org/ver20/ptz/wsdl/RelativeMove",
             "<tptz:RelativeMove><tptz:ProfileToken>" + xml_escape(profile_token_) +
                 "</tptz:ProfileToken><tptz:Translation><tt:PanTilt x=\"" + format_number(x) + "\" y=\"" +
                 format_number(y) + "\" space=\"" + kFovSpace + "\"/></tptz:Translation></tptz:RelativeMove>",
             "tptz");
    }

private:
    void sync_clock() {
        // WS-Security timestamps are checked by the camera; follow its clock.
        try {
            auto body = call(base_ + "/onvif/device_service", kDeviceNs,
                             "http://www.onvif.org/ver10/device/wsdl/GetSystemDateAndTime",
                             "<tds:GetSystemDateAndTime/>", "tds", false);
            const XmlNode *utc = body.find("UTCDateTime");
            if (!utc) return;
            const XmlNode *date = utc->child("Date"), *time = utc->child("Time");
            if (!date || !time) return;
            std::tm tm{};
            tm.tm_year = std::stoi(date->child_text("Year")) - 1900;
            tm.tm_mon = std::stoi(date->child_text("Month")) - 1;
            tm.tm_mday = std::stoi(date->child_text("Day"));
            tm.tm_hour = std::stoi(time->child_text("Hour"));
            tm.tm_min = std::stoi(time->child_text("Minute"));
            tm.tm_sec = std::stoi(time->child_text("Second"));
#if defined(_WIN32)
            std::time_t camera = _mkgmtime(&tm);
#else
            std::time_t camera = timegm(&tm);
#endif
            offset_ = std::chrono::seconds(camera - std::time(nullptr));
            if (std::chrono::abs(offset_) > std::chrono::seconds(30) && hooks_.log)
                hooks_.log("Camera clock differs by " + std::to_string(offset_.count()) +
                           " s; WS-Security timestamps follow the camera");
        } catch (...) {
            offset_ = {};
        }
    }

    void discover_services() {
        std::string device = base_ + "/onvif/device_service";
        try {
            auto body = call(device, kDeviceNs, "http://www.onvif.org/ver10/device/wsdl/GetCapabilities",
                             "<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>", "tds");
            if (auto media = body.find("Media")) media_ = media->child_text("XAddr");
            if (auto ptz = body.find("PTZ")) ptz_ = ptz->child_text("XAddr");
        } catch (const Error &) {
            auto body = call(device, kDeviceNs, "http://www.onvif.org/ver10/device/wsdl/GetServices",
                             "<tds:GetServices><tds:IncludeCapability>false</tds:IncludeCapability></tds:GetServices>",
                             "tds");
            for (auto service : body.children_named("Service")) {
                std::string ns = service->child_text("Namespace");
                if (ns == kMediaNs) media_ = service->child_text("XAddr");
                if (ns == kPtzNs) ptz_ = service->child_text("XAddr");
            }
        }
        if (media_.empty()) throw Error("Camera does not advertise an ONVIF media service");
        if (ptz_.empty()) throw Error("Camera does not advertise an ONVIF PTZ service");
        media_ = route(media_);
        ptz_ = route(ptz_);
    }

    std::string route(const std::string &xaddr) const {
        // Keep the service path; when tunnelled, always dial the forwarded port.
        auto scheme_end = xaddr.find("://");
        if (scheme_end == std::string::npos) throw Error("Camera advertised an invalid service address");
        std::string scheme = lower(xaddr.substr(0, scheme_end));
        auto path_start = xaddr.find('/', scheme_end + 3);
        std::string path = path_start == std::string::npos ? "/" : xaddr.substr(path_start);
        if (!p_.tunneled) return xaddr;
        if (scheme != "http") throw Error("SSH mode requires camera ONVIF services over HTTP");
        return base_ + path;
    }

    std::string security_header() const {
        auto nonce = random_bytes(16);
        std::string created = iso_utc(std::chrono::system_clock::now() + offset_);
        std::string material = nonce + created + p_.password;
        unsigned char digest[20];
        sha1(reinterpret_cast<const unsigned char *>(material.data()), material.size(), digest);
        return "<s:Header><wsse:Security s:mustUnderstand=\"1\" "
               "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
               "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
               "<wsse:UsernameToken><wsse:Username>" + xml_escape(p_.user) + "</wsse:Username>"
               "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
               "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">" + base64_encode(digest, 20) +
               "</wsse:Password><wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
               "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">" +
               base64_encode(reinterpret_cast<const unsigned char *>(nonce.data()), nonce.size()) +
               "</wsse:Nonce><wsu:Created>" + created + "</wsu:Created></wsse:UsernameToken></wsse:Security></s:Header>";
    }

    XmlNode call(const std::string &url, const char *ns, const char *action, const std::string &payload,
                 const char *prefix, bool authenticate = true) {
        std::string envelope = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") +
                               "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:" + prefix +
                               "=\"" + ns + "\" xmlns:tt=\"" + kSchemaNs + "\">" +
                               (authenticate ? security_header() : "") + "<s:Body>" + payload +
                               "</s:Body></s:Envelope>";
        HttpRequest request;
        request.method = "POST";
        request.url = url;
        request.body = envelope;
        request.headers = {std::string("Content-Type: application/soap+xml; charset=utf-8; action=\"") + action + "\""};
        request.timeout = p_.timeout;
        // Some firmware also wants HTTP Digest in front of the SOAP service. Only
        // use it once challenged: curl's Digest probe sends an empty POST body.
        auto with_digest = [&] {
            request.digest_auth = true;
            request.user = p_.user;
            request.password = p_.password;
        };
        if (authenticate && http_digest_) with_digest();
        HttpResponse response = http_perform(request);
        if (authenticate && !http_digest_ && response.transport_ok && response.status == 401 &&
            lower(response.headers["www-authenticate"]).find("digest") != std::string::npos) {
            http_digest_ = true;
            with_digest();
            response = http_perform(request);
        }
        if (!response.transport_ok) throw Error("Camera unreachable: " + response.transport_error);
        XmlNode document;
        try {
            document = xml_parse(response.body);
        } catch (const Error &) {
            if (response.status == 401) throw Error("Camera rejected the credentials (HTTP 401)");
            throw Error("Camera returned HTTP " + std::to_string(response.status) + " without a SOAP response");
        }
        const XmlNode *body = document.child("Body");
        if (!body) throw Error("Camera response has no SOAP body");
        if (const XmlNode *fault = body->child("Fault")) {
            std::string reason;
            if (auto r = fault->find("Reason")) reason = r->find("Text") ? r->find("Text")->text : r->text;
            if (reason.empty()) reason = fault->child_text("faultstring");
            std::string code;
            if (auto sub = fault->find("Subcode")) code = sub->child_text("Value");
            if (reason.empty()) reason = "SOAP fault";
            if (code.find("NotAuthorized") != std::string::npos || response.status == 401)
                reason = "Camera rejected the credentials (" + reason +
                         "); check the camera's ONVIF username/password and that its clock is set";
            throw Error(printable(reason));
        }
        if (response.status >= 400) throw Error("Camera returned HTTP " + std::to_string(response.status));
        if (body->children.empty()) throw Error("Camera returned an empty SOAP body");
        return body->children.front();
    }

    CameraParams p_;
    const Hooks &hooks_;
    std::string base_, media_, ptz_, profile_token_;
    std::chrono::seconds offset_{0};
    bool http_digest_ = false;
    Space pan_tilt_;
    std::optional<Space> zoom_;
    std::optional<Space> fov_;
    std::string snapshot_uri_;
};

}  // namespace

std::unique_ptr<Camera> make_onvif(const CameraParams &params, const Hooks &hooks) {
    return std::make_unique<Onvif>(params, hooks);
}

}  // namespace ptz
