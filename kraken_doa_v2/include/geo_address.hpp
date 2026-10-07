#pragma once

// Street addresses in free text (pager messages) -> positions, looked up
// online in OpenStreetMap (Nominatim, geo_http.hpp) for the incident map
// (incidents.cpp). No local street data.
//
// Finding the address ("general fallback", no per-agency templates): the
// text is folded to upper-case ASCII words; candidate streets come from
//   - a type word after 1-4 name words: "12 QUEEN ST", "123 N MAIN ST",
//     "350 5TH AVE", "QUEEN ST W", "BERLINER STR 5", "KARL JOHANS GATE 22"
//   - a type word before 1-4 name words: "RUE DE LA PAIX", "CALLE MAYOR 1",
//     "VIA DEL CORSO 100", "RUA AUGUSTA", "UL MARSZALKOWSKA 10", "JL ..."
//   - one word with the type joined on: "FRIEDRICHSTR 43", "PRINSENGRACHT
//     263", "DROTTNINGGATAN 50", "VESTERGADE 10", "MANNERHEIMINTIE 5"
// with the house number before or after the street. Names stop at a number,
// a , ; ( ) or a word that can't be part of one (VS, NEAR, FIRE, ...); the
// longest name is tried first. Candidates with a house number go first.
// Latin-script languages only (other scripts are not read).
// Each candidate is searched within radius_km of the station, and a result
// counts only if its road name is the candidate's street. Between several
// results: the one with the house number, then one whose suburb / town /
// city is named in the text, then the junction with a cross street named in
// the text, then the one nearest the station.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace geo {

double distance_km(double lat1, double lon1, double lat2, double lon2);

// Upper-case ASCII words: accents folded (Ō -> O, ß -> SS, Æ -> AE),
// apostrophes dropped (O'Rorke -> ORORKE), other punctuation separates words.
// keep_numbers: "12/3" and "3-5" stay one word (house numbers in messages).
// breaks (optional): per word, whether a , ; : ( ) came before it
std::vector<std::string> words(const std::string& text, bool keep_numbers = false, std::vector<bool>* breaks = nullptr);
// Comparison key of a street name (the message's words and OSM's names go
// through the same function): street types before / after / joined to the
// name in a short form (Street / Straße / Str -> ST, Avenue / Avenida -> AV
// before a name, Friedrichstraße -> FRIEDRICHST), direction words N S E W,
// SAINT -> ST, MOUNT -> MT, FIFTH -> 5TH; Polish UL. dropped
std::string street_key(const std::string& name);
std::string place_key(const std::string& name);

// --- a minimal JSON reader (the Nominatim answers) ---
struct Json {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;
    const Json* get(const std::string& key) const;
    std::string str(const std::string& key) const;   // "" if missing / not a string
};
bool json_parse(const std::string& text, Json* out);

struct Result {
    double lat = 0, lon = 0;
    std::string address;      // "12 Queen Street"
    std::string street;       // the OSM road name
    std::string number;       // "" = none in the text
    std::string cross;        // cross street (OSM name), "" = none
    std::string place;        // suburb / town named in the text that decided, "" = none
    std::string area;         // the result's suburb, town (context for the popup)
    std::string precision;    // "address" (the house) | "junction" | "street" (a point on the road)
    std::string confidence;   // "high" | "medium" | "low"
    int alternatives = 0;     // other streets of that name further away
    std::string key;          // street key + number (incident de-duplication)
    int lookups = 0;          // searches made (incl. cached answers)
};

// One search: free-text query within the box s,w,n,e -> the JSON answer
// (Nominatim jsonv2 with addressdetails). false + err on failure; *offline
// = no answer at all (no internet) as opposed to an error answer.
using SearchFn = std::function<bool(const std::string& query, double s, double w, double n, double e,
                                    std::string* json, std::string* err, bool* offline)>;

// The address in text, positioned. false: none found (err empty), or the
// lookup failed (err set; *offline = worth trying again later).
bool geocode(const std::string& text, double st_lat, double st_lon, double radius_km, const SearchFn& search,
             Result* r, std::string* err, bool* offline);

}  // namespace geo
