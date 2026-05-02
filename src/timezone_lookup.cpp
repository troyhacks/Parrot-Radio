#include "timezone_lookup.h"

// World timezone regions - "close enough" bounding boxes
// Format: { minLat, maxLat, minLon, maxLon, POSIX string }
// Overlap handling: first matching region wins (order matters!)

static const TZRegion regions[] = {
  // ===== UTC-12 to UTC-14 (west of GMT) =====
  { -15.0f, 0.0f, -180.0f, -170.0f, "UTC0" },         // Kiribati Line Islands, UTC+14
  { -15.0f, -5.0f, -180.0f, -172.0f, "UTC0" },         // Kiribati (Phoenix), UTC+13
  { -30.0f, -15.0f, -180.0f, -172.0f, "UTC0" },         // Samoa, Tonga, UTC+13
  { -5.0f, 5.0f, 172.0f, 180.0f, "UTC0" },              // Nauru, Kiribati (Gilbert), UTC+12
  { -50.0f, -30.0f, 172.0f, 180.0f, "UTC0" },           // New Zealand (Chatham), UTC+12:45

  // ===== UTC-12:00 (Baker Island, Howland) =====
  { 0.0f, 5.0f, -180.0f, -175.0f, "UTC0" },            // Baker/Howland area

  // ===== UTC-11:00 (American Samoa, Niue) =====
  { -15.0f, -10.0f, -180.0f, -165.0f, "UTC0" },         // American Samoa, UTC-11

  // ===== UTC-10:00 (Hawaii, French Polynesia) =====
  { 18.0f, 30.0f, -160.0f, -145.0f, "HST10" },          // Hawaii
  { -30.0f, -10.0f, -155.0f, -145.0f, "HST10" },        // French Polynesia (Tahiti area)

  // ===== UTC-9:00 (Alaska) =====
  { 55.0f, 72.0f, -170.0f, -130.0f, "AKST9AKDT,M3.2.0,M11.1.0" },  // Alaska

  // ===== UTC-8:00 (Pacific - US West Coast, Canada) =====
  { 30.0f, 55.0f, -130.0f, -117.0f, "PST8PDT,M3.2.0,M11.1.0" },    // Pacific US/Canada

  // ===== UTC-7:00 (Mountain - US, Canada, Mexico) =====
  { 30.0f, 50.0f, -117.0f, -102.0f, "MST7MDT,M3.2.0,M11.1.0" },    // Mountain US/Canada
  { 25.0f, 35.0f, -118.0f, -109.0f, "MST7" },                       // Arizona (no DST)
  { 25.0f, 50.0f, -117.0f, -102.0f, "MST7" },                        // Mexican states in MST

  // ===== UTC-6:00 (Central - US, Canada, Mexico, Central America) =====
  { 15.0f, 30.0f, -118.0f, -85.0f, "CST6CDT,M3.2.0,M11.1.0" },    // Mexico, Central America (select)
  { 25.0f, 50.0f, -102.0f, -87.0f, "CST6CDT,M3.2.0,M11.1.0" },    // Central US/Canada
  { 49.5f, 60.0f, -110.0f, -102.0f, "CST6" },                      // Saskatchewan (no DST)

  // ===== Newfoundland (UTC-3:30 / UTC-2:30) =====
  { 46.0f, 52.0f, -59.0f, -52.0f, "NST3:30NDT,M3.2.0,M11.1.0" },  // Newfoundland & Labrador

  // ===== UTC-5:00 (Eastern - US/Canada, South America) =====
  { 24.0f, 50.0f, -87.0f, -65.0f, "EST5EDT,M3.2.0,M11.1.0" },     // Eastern US/Canada
  { 5.0f, 25.0f, -82.0f, -65.0f, "EST5EDT,M3.2.0,M11.1.0" },      // Panama, Caribbean
  { -5.0f, 15.0f, -82.0f, -60.0f, "COT5" },                        // Colombia, Ecuador (mainland)
  { -20.0f, 5.0f, -80.0f, -60.0f, "PET5" },                         // Peru, Ecuador (south), Bolivia, Venezuela

  // ===== UTC-4:00 (Atlantic - Canada, South America) =====
  { 35.0f, 55.0f, -68.0f, -52.0f, "AST4" },                        // Atlantic Canada (no DST)
  { -30.0f, -15.0f, -60.0f, -45.0f, "GMT3" },                       // Argentina, Uruguay (rough)
  { -30.0f, -20.0f, -60.0f, -50.0f, "PYT4" },                       // Paraguay
  { -55.0f, -10.0f, -75.0f, -35.0f, "GMT3" },                       // Brazil (east, including São Paulo)

  // ===== UTC-3:00 (Brazil, Greenland, etc.) =====
  { -30.0f, -10.0f, -50.0f, -35.0f, "BRT3" },                       // Brazil (Brasilia time)

  // ===== UTC-2:00 (Mid-Atlantic) =====
  { -50.0f, -20.0f, -40.0f, -15.0f, "GMT2" },                       // South Georgia, Brazil (islands)

  // ===== UTC-1:00 (Azores, Cape Verde) =====
  { 35.0f, 45.0f, -35.0f, -20.0f, "AZOT1" },                        // Azores
  { 10.0f, 25.0f, -30.0f, -15.0f, "CVT1" },                         // Cape Verde

  // ===== UTC+0 (GMT/UTC - UK, W. Africa, Iceland) =====
  { 49.0f, 62.0f, -10.0f, 0.0f, "GMT0BST,M3.5.0,M10.5.0" },       // UK/Ireland
  { 50.0f, 65.0f, -30.0f, -10.0f, "GMT0" },                        // Iceland, E. Greenland
  { 12.0f, 35.0f, -20.0f, 0.0f, "GMT0" },                          // W. Africa (Mauritania to Nigeria)
  { 0.0f, 12.0f, -20.0f, 0.0f, "GMT0" },                           // Guinea to Ethiopia (rough)
  { 0.0f, 12.0f, 20.0f, 35.0f, "EAT3" },                           // E. Africa

  // ===== UTC+1 (CET - W. Europe, W. Africa) =====
  { 35.0f, 50.0f, -10.0f, 15.0f, "CET1CEST,M3.5.0,M10.5.0" },     // W. Europe (France, Germany, Spain, Italy)
  { 50.0f, 60.0f, 5.0f, 15.0f, "CET1CEST,M3.5.0,M10.5.0" },      // S. Scandinavia (rough)
  { 35.0f, 50.0f, 15.0f, 30.0f, "EET2" },                          // Poland to Romania

  // ===== UTC+2 (EET - E. Europe, Egypt, Israel, S. Africa) =====
  { 25.0f, 35.0f, 25.0f, 35.0f, "EET2EEST,M3.5.0,M10.5.0" },      // Egypt, Israel, Jordan, Lebanon
  { 25.0f, 40.0f, 35.0f, 45.0f, "EET2EEST,M3.5.0,M10.5.0" },      // Syria, Iraq, Kuwait
  { 22.0f, 40.0f, 45.0f, 60.0f, "AFT4" },                           // Afghanistan (rough)
  { 35.0f, 45.0f, 45.0f, 60.0f, "EET2" },                           // Iran (NW parts)
  { -35.0f, -20.0f, 15.0f, 35.0f, "SAST2" },                        // S. Africa, Namibia, Botswana, Zimbabwe

  // ===== UTC+3 (Moscow, E. Africa, Middle East) =====
  { 30.0f, 45.0f, 35.0f, 45.0f, "EET2EEST,M3.5.0,M10.5.0" },      // Turkey, Cyprus, Greece, Bulgaria
  { 35.0f, 72.0f, 45.0f, 60.0f, "MSK4" },                           // Russia (Moscow area)
  { 12.0f, 30.0f, 35.0f, 50.0f, "EAT3" },                           // E. Africa (Kenya, Tanzania, Ethiopia)
  { 15.0f, 35.0f, 50.0f, 60.0f, "AST3" },                           // Arabian Peninsula (Saudi, Yemen, Oman)

  // ===== UTC+3:30 (Iran) =====
  { 25.0f, 40.0f, 45.0f, 53.0f, "IRST3:30" },                      // Iran

  // ===== UTC+4 (Dubai, Russia West, Afghanistan SE) =====
  { 10.0f, 35.0f, 50.0f, 60.0f, "GST4" },                          // UAE, Oman, Bahrain, Qatar
  { 35.0f, 72.0f, 50.0f, 60.0f, "AFT4" },                           // Afghanistan, Turkmenistan, Uzbekistan
  { 40.0f, 72.0f, 60.0f, 75.0f, "TJT5" },                           // Tajikistan, Kyrgyzstan, Kazakhstan

  // ===== UTC+4:30 (Afghanistan, Iran SE) =====
  { 25.0f, 35.0f, 60.0f, 67.0f, "AFT4:30" },                        // Afghanistan SE, Pakistan W

  // ===== UTC+5 (Pakistan, Maldives, C. Asia) =====
  { 20.0f, 40.0f, 60.0f, 70.0f, "PKT5" },                            // Pakistan, Maldives
  { 35.0f, 45.0f, 65.0f, 75.0f, "TJT5" },                           // Kazakhstan W, Uzbekistan, Turkmenistan
  { 40.0f, 50.0f, 70.0f, 80.0f, "ALMT6" },                          // Kazakhstan E, Kyrgyzstan

  // ===== UTC+5:30 (India, Sri Lanka) =====
  { 5.0f, 35.0f, 65.0f, 80.0f, "IST5:30" },                         // India, Sri Lanka

  // ===== UTC+5:45 (Nepal) =====
  { 26.0f, 30.0f, 80.0f, 88.0f, "NPT5:45" },                        // Nepal

  // ===== UTC+6 (Bangladesh, Bhutan, Russia Siberia W) =====
  { 20.0f, 30.0f, 85.0f, 93.0f, "BDT6" },                            // Bangladesh, Bhutan
  { 40.0f, 55.0f, 75.0f, 90.0f, "ALMT6" },                           // Kazakhstan E, Russia (Siberia W)
  { -25.0f, -15.0f, 45.0f, 55.0f, "EAT3" },                          // Madagascar (rough)

  // ===== UTC+6:30 (Myanmar, Cocos) =====
  { 10.0f, 28.0f, 92.0f, 102.0f, "MMT6:30" },                        // Myanmar, Thailand (east), Laos

  // ===== UTC+7 (Thailand, Vietnam, Laos, Russia Siberia E) =====
  { 5.0f, 25.0f, 97.0f, 110.0f, "ICT7" },                            // Thailand, Vietnam, Cambodia, Laos
  { 50.0f, 80.0f, 75.0f, 105.0f, "NOVT7" },                          // Russia (Siberia), Mongolia

  // ===== UTC+8 (China, Taiwan, HK, Singapore, Perth, Indonesia W) =====
  { 18.0f, 55.0f, 110.0f, 125.0f, "CST8" },                          // China, Taiwan, Hong Kong, Mongolia E
  { -35.0f, -25.0f, 112.0f, 118.0f, "WST8" },                        // Western Australia (Perth area)
  { 0.0f, 10.0f, 95.0f, 110.0f, "ICT7" },                            // Indonesia (Sumatra, Java W)
  { -10.0f, 0.0f, 105.0f, 110.0f, "WITA8" },                         // Indonesia (Borneo W, Sulawesi)

  // ===== UTC+8:45 (Australia - Eucla, parts of SA) =====
  { -35.0f, -30.0f, 128.0f, 135.0f, "CWST8:45" },                    // Australia (Eucla, Rawlinna)

  // ===== UTC+9 (Japan, Korea, Russia Far East, Indonesia E) =====
  { 30.0f, 50.0f, 125.0f, 145.0f, "JST9" },                          // Japan, Korea, Russia (Vladivostok)
  { -10.0f, 0.0f, 110.0f, 125.0f, "WITA8" },                         // Indonesia (Kalimantan, Sulawesi)
  { -10.0f, 0.0f, 125.0f, 140.0f, "WIT9" },                           // Indonesia (Papua, Maluku)

  // ===== UTC+9:30 (Australia - Darwin, Adelaide) =====
  { -35.0f, -12.0f, 128.0f, 140.0f, "ACST9:30" },                    // Australia (NT, SA)

  // ===== UTC+10 (Australia E, Papua New Guinea, Russia Far East) =====
  { -45.0f, -35.0f, 135.0f, 155.0f, "AEST10" },                      // Australia (NSW, VIC, QLD, ACT)
  { -10.0f, 0.0f, 140.0f, 155.0f, "PGT10" },                         // Papua New Guinea
  { 40.0f, 65.0f, 140.0f, 180.0f, "VLAT10" },                        // Russia (Sakhalin, Kamchatka, Magadan)

  // ===== UTC+10:30 (Australia - Lord Howe) =====
  { -35.0f, -30.0f, 150.0f, 160.0f, "LHST10:30" },                    // Lord Howe Island

  // ===== UTC+11 (Solomon Islands, Vanuatu, New Caledonia) =====
  { -25.0f, -10.0f, 155.0f, 170.0f, "SBT11" },                       // Solomon Islands, Vanuatu
  { -25.0f, -20.0f, 165.0f, 170.0f, "NCT11" },                        // New Caledonia

  // ===== UTC+12 (New Zealand, Fiji, Russia Far East) =====
  { -50.0f, -30.0f, 165.0f, 180.0f, "NZST12" },                      // New Zealand (main)
  { -30.0f, -15.0f, 175.0f, 180.0f, "NZST12" },                      // Fiji (rough)
  { 60.0f, 65.0f, 160.0f, 180.0f, "PETT12" },                        // Russia (Kamchatka, Chukotka)

  // ===== UTC+12:45 (Chatham Islands) =====
  { -50.0f, -42.0f, -180.0f, -175.0f, "CHAST12:45" },                // Chatham Islands

  // ===== UTC+13 (Tonga, parts of Samoa, Phoenix Islands) =====
  { -25.0f, -15.0f, -180.0f, -172.0f, "TOT13" },                     // Tonga
  { -15.0f, -10.0f, -180.0f, -170.0f, "TOT13" },                     // Phoenix Islands (Kiribati)
  { -25.0f, -20.0f, -175.0f, -168.0f, "TOT13" },                     // Samoa (rough)

  // ===== UTC+14 (Line Islands - Kiribati) =====
  { -10.0f, 0.0f, -180.0f, -170.0f, "LINT14" },                       // Kiribati (Line Islands)
};

const char* getTimezoneForCoords(float lat, float lon) {
  // Normalize longitude to -180 to +180
  while (lon > 180.0f) lon -= 360.0f;
  while (lon < -180.0f) lon += 360.0f;

  for (const auto& r : regions) {
    if (lat >= r.minLat && lat <= r.maxLat &&
        lon >= r.minLon && lon <= r.maxLon) {
      return r.posix;
    }
  }

  // Special cases: Russia spans many zones, fallback to UTC offset based on lon
  if (lat > 60.0f && lon > 60.0f && lon < 180.0f) {
    // Russia - rough estimate by longitude
    if (lon < 90.0f) return "OMST6";        // Omsk
    if (lon < 120.0f) return "KRAT8";       // Krasnoyarsk
    if (lon < 150.0f) return "IRKT8";      // Irkutsk
    if (lon < 180.0f) return "VLAT10";      // Vladivostok
  }

  // Antarctica (rough - most research stations use their country's timezone or UTC)
  if (lat < -60.0f) return "UTC0";

  return nullptr;  // Unknown
}
