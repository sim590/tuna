/*************************************************************************
 * This file is part of tuna
 * github.con/univrsal/tuna
 * Copyright 2020 univrsal <universailp@web.de>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *************************************************************************/

#include "spotify_source.hpp"
#include "../gui/tuna_gui.hpp"
#include "../util/config.hpp"
#include "../util/constants.hpp"
#include "../util/creds.hpp"
#include "../util/utility.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <curl/curl.h>
#include <util/config-file.h>
#include <util/platform.h>

#define TOKEN_URL "https://accounts.spotify.com/api/token"
#define PLAYER_URL "https://api.spotify.com/v1/me/player"
#define REDIRECT_URI "https%3A%2F%2Funivrsal.github.io%2Fauth%2Ftoken"

spotify_source::spotify_source()
{
    /* builds credentials for spotify api */
    QString str = SPOTIFY_CREDENTIALS;
    m_creds = str.toUtf8().toBase64();
    m_capabilities = CAP_TITLE | CAP_ARTIST | CAP_ALBUM | CAP_RELEASE | CAP_COVER | CAP_DURATION | CAP_NEXT_SONG | CAP_PREV_SONG | CAP_PLAY_PAUSE | CAP_VOLUME_UP | CAP_VOLUME_DOWN | CAP_VOLUME_MUTE | CAP_PREV_SONG | CAP_STATUS;
}

void spotify_source::load()
{
    CDEF_BOOL(CFG_SPOTIFY_LOGGEDIN, false);
    CDEF_STR(CFG_SPOTIFY_TOKEN, "");
    CDEF_STR(CFG_SPOTIFY_AUTH_CODE, "");
    CDEF_STR(CFG_SPOTIFY_REFRESH_TOKEN, "");
    CDEF_INT(CFG_SPOTIFY_TOKEN_TERMINATION, 0);

    m_logged_in = CGET_BOOL(CFG_SPOTIFY_LOGGEDIN);
    m_token = utf8_to_qt(CGET_STR(CFG_SPOTIFY_TOKEN));
    m_refresh_token = utf8_to_qt(CGET_STR(CFG_SPOTIFY_REFRESH_TOKEN));
    m_auth_code = utf8_to_qt(CGET_STR(CFG_SPOTIFY_AUTH_CODE));
    m_token_termination = CGET_INT(CFG_SPOTIFY_TOKEN_TERMINATION);

    /* Token handling */
    if (m_logged_in) {
        if (util::epoch() > m_token_termination) {
            QString log;
            bool result = do_refresh_token(log);
            if (result) {
                bdebug("Successfully renewed Spotify token");
            }
            save();
        }
    }
}

void spotify_source::load_gui_values()
{
    tuna_dialog->set_spotify_auth_code(m_auth_code);
    tuna_dialog->set_spotify_auth_token(m_token);
    tuna_dialog->set_spotify_refresh_token(m_refresh_token);
}

void spotify_source::save()
{
    CSET_BOOL(CFG_SPOTIFY_LOGGEDIN, m_logged_in);
    CSET_STR(CFG_SPOTIFY_TOKEN, qt_to_utf8(m_token));
    CSET_STR(CFG_SPOTIFY_AUTH_CODE, qt_to_utf8(m_auth_code));
    CSET_STR(CFG_SPOTIFY_REFRESH_TOKEN, qt_to_utf8(m_refresh_token));
    CSET_INT(CFG_SPOTIFY_TOKEN_TERMINATION, m_token_termination);
}

bool spotify_source::valid_format(const QString& str)
{
    /* Supports all specifiers */
    return true;
}

/* implementation further down */
long execute_command(const char* auth_token, const char* url, std::string& response_header,
    QJsonDocument& response_json);

void extract_timeout(const std::string& header, uint64_t& timeout)
{
    static const std::string what = "Retry-After: ";
    timeout = 0;
    size_t pos = header.find(what);
    size_t end;

    if (pos != std::string::npos) {
        pos += what.length();
        end = pos;
        std::string tmp;
        while (header.at(end) != '\n')
            end++;
        tmp = header.substr(pos, end - pos);
        timeout = std::stoi(tmp);
    }
}

void spotify_source::refresh()
{
    if (!m_logged_in)
        return;

    if (util::epoch() > m_token_termination) {
        QString log;
        bool result = do_refresh_token(log);
        tuna_dialog->apply_login_state(result, log);
        save();
    }

    if (m_timout_start) {
        if (os_gettime_ns() - m_timout_start >= m_timeout_length) {
            m_timout_start = 0;
            m_timeout_length = 0;
        } else {
            binfo("Waiting for Spotify-API timeout");
            return;
        }
    }

    std::string header = "";
    QJsonDocument response;
    QJsonObject obj;

    auto http_code = execute_command(m_token.toStdString().c_str(), PLAYER_URL, header, response);

    if (response.isObject())
        obj = response.object();
    QString str(response.toJson());

    if (http_code == 200) {
        const auto& progress = obj["progress_ms"];
        const auto& device = obj["device"];
        const auto& playing = obj["is_playing"];

        if (device.isObject() && playing.isBool()) {
            if (device.toObject()["is_private"].toBool()) {
                berr("Spotify session is private! Can't read track");
            } else {
                parse_track_json(obj["item"]);
                m_current.set_playing(playing.toBool());
            }
            m_current.set_progress(progress.toInt());
        } else {
            QString str(response.toJson());
            berr("Couldn't fetch song data from spotify json: %s", str.toStdString().c_str());
        }
    } else {
        if (http_code == STATUS_RETRY_AFTER && !header.empty()) {
            extract_timeout(header, m_timeout_length);
            if (m_timeout_length) {
                bwarn("Spotify-API Rate limit hit, waiting %ull seconds\n", m_timeout_length);
                m_timeout_length *= SECOND_TO_NS;
                m_timout_start = os_gettime_ns();
            }
        } else {
            bwarn("Unknown error occured when querying Spotify-API: %i (response: %s)", http_code,
                str.toStdString().c_str());
        }
    }
}

void spotify_source::parse_track_json(const QJsonValue& track)
{
    const auto& album = track["album"];
    const auto& artists = track["artists"];

    if (album.isObject() && artists.isArray()) {
        m_current.clear();

        /* Get All artists */
        for (const auto artist : artists.toArray())
            m_current.append_artist(artist.toObject()["name"].toString());

        /* Cover link */
        const auto& covers = album["images"];
        if (covers.isArray()) {
            QJsonValue v = covers.toArray()[0];
            if (v.isObject() && v.toObject().contains("url"))
                m_current.set_cover_link(v.toObject()["url"].toString());
        }

        /* Other stuff */
        m_current.set_title(track["name"].toString());
        m_current.set_duration(track["duration_ms"].toInt());
        m_current.set_album(album["name"].toString());
        m_current.set_explicit(track["explicit"].toBool());
        m_current.set_disc_number(track["disc_number"].toInt());
        m_current.set_track_number(track["track_number"].toInt());

        /* Release date */
        const auto& date = album["release_date"].toString();
        if (date.length() > 0) {
            QStringList list = date.split("-");
            switch (list.length()) {
            case 3:
                m_current.set_day(list[2]);
            case 2: /* Fallthrough */
                m_current.set_month(list[1]);
            case 1: /* Fallthrough */
                m_current.set_year(list[0]);
            default:;
            }
        }
    }
}

bool spotify_source::execute_capability(capability c)
{
    bool result = true;
    switch (c) {
    case CAP_NEXT_SONG:
        break;
    case CAP_PREV_SONG:
        break;
    case CAP_PLAY_PAUSE:
        break;
    case CAP_VOLUME_UP:
        break;
    case CAP_VOLUME_DOWN:
        break;
    case CAP_VOLUME_MUTE:
        break;
    default:;
    }
    return result;
}

/* === CURL/Spotify API handling === */

size_t write_callback(char* ptr, size_t size, size_t nmemb, std::string* str)
{
    size_t new_length = size * nmemb;
    try {
        str->append(ptr, new_length);
    } catch (std::bad_alloc& e) {
        berr("Error reading curl response: %s", e.what());
        return 0;
    }
    return new_length;
}

size_t header_callback(char* ptr, size_t size, size_t nmemb, std::string* str)
{
    size_t new_length = size * nmemb;
    try {
        str->append(ptr, new_length);
    } catch (std::bad_alloc& e) {
        berr("Error reading curl header: %s", e.what());
        return 0;
    }
    return new_length;
}

CURL* prepare_curl(struct curl_slist* header, std::string* response, std::string* response_header,
    const std::string& request)
{
    CURL* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, TOKEN_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(request.c_str()));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, response_header);
#ifdef DEBUG
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
#endif
    return curl;
}

/* Requests an access token via request body
 * over a POST request to spotify */
void request_token(const std::string& request, const std::string& credentials, QJsonDocument& response_json)
{
    if (request.empty() || credentials.empty()) {
        berr("Cannot request token without valid credentials"
             " and/or auth code!");
        return;
    }

    std::string response, response_header;
    std::string header = "Authorization: Basic ";
    header.append(credentials);

    auto* list = curl_slist_append(nullptr, header.c_str());
    CURL* curl = prepare_curl(list, &response, &response_header, request);
    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        QJsonParseError err;
        response_json = QJsonDocument::fromJson(response.c_str(), &err);
        if (response_json.isNull()) {
            berr("Couldn't parse response to json: %s",
                err.errorString().toStdString().c_str());
        } else {
            /* Log response without tokens */
            auto obj = response_json.object();
            obj["access_token"] = "REDACTED";
            obj["refresh_token"] = "REDACTED";
            auto doc = QJsonDocument(obj);
            QString str(doc.toJson());
            binfo("Spotify response: %s", qt_to_utf8(str));
        }
    } else {
        berr("Curl returned error code %i", res);
    }

    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
}

/* Gets a new token using the refresh token */
bool spotify_source::do_refresh_token(QString& log)
{
    static std::string request;
    bool result = true;
    QJsonDocument response;
    request = "grant_type=refresh_token&refresh_token=";
    request.append(m_refresh_token.toStdString());
    request_token(request, m_creds.toStdString(), response);

    if (response.isNull()) {
        return false;
    } else {
        const auto& token = response["access_token"];
        const auto& expires = response["expires_in"];
        const auto& refresh_token = response["refresh_token"];

        /* Dump the json into the log text */
        log = QString(response.toJson(QJsonDocument::Indented));
        if (!token.isNull() && !expires.isNull()) {
            m_token = token.toString();
            m_token_termination = util::epoch() + expires.toInt();
            m_logged_in = true;
            save();
        } else {
            berr("Couldn't parse json response");
            result = false;
        }

        /* Refreshing the token can return a new refresh token */
        if (!refresh_token.isNull())
            m_refresh_token = refresh_token.toString();
    }

    m_logged_in = result;
    return result;
}

/* Gets the first token from the access code */
bool spotify_source::new_token(QString& log)
{
    static std::string request;
    bool result = true;
    QJsonDocument response;
    request = "grant_type=authorization_code&code=";
    request.append(m_auth_code.toStdString());
    request.append("&redirect_uri=").append(REDIRECT_URI);
    request_token(request, m_creds.toStdString(), response);

    if (response.isObject()) {
        const auto& token = response["access_token"];
        const auto& refresh = response["refresh_token"];
        const auto& expires = response["expires_in"];

        /* Dump the json into the log textbox */
        log = QString(response.toJson(QJsonDocument::Indented));

        if (token.isString() && refresh.isString() && expires.isDouble()) {
            m_token = token.toString();
            m_refresh_token = refresh.toString();
            m_token_termination = util::epoch() + expires.toInt();
            result = true;
        } else {
            berr("Couldn't parse json response!");
            result = false;
        }
    } else {
        result = false;
    }

    m_logged_in = result;
    save();
    return result;
}

/* Sends commands to spotify api via url */

long execute_command(const char* auth_token, const char* url, std::string& response_header,
    QJsonDocument& response_json)
{
    std::string response;
    std::string header = "Authorization: Bearer ";
    long http_code = -1;
    header.append(auth_token);
    auto* list = curl_slist_append(nullptr, header.c_str());

    CURL* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_header);
    if (!response_header.empty())
        bdebug("Response header: %s", response_header.c_str());

#ifdef DEBUG
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
#endif
    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        QJsonParseError err;

        response_json = QJsonDocument::fromJson(response.c_str(), &err);
        if (response_json.isNull() && !response.empty())
            berr("Failed to parse json response: %s, Error: %s", response.c_str(),
                qt_to_utf8(err.errorString()));
    } else {
        berr("CURL failed while sending spotify command");
    }

    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return http_code;
}
