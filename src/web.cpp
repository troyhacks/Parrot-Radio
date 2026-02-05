#include "web.h"
#include "config.h"
#include "rtc.h"
#include <WiFi.h>
#include <time.h>

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><title>Radio Parrot</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;margin:20px;max-width:400px;}";
  html += "input,select{margin:5px 0;padding:8px;width:100%;box-sizing:border-box;}";
  html += ".status{padding:10px;margin:10px 0;border-radius:5px;}";
  html += ".connected{background:#d4edda;}.disconnected{background:#f8d7da;}";
  html += ".btn{background:#007bff;color:white;border:none;padding:10px;cursor:pointer;margin:5px 0;}";
  html += ".coords{display:flex;gap:10px;}.coords input{width:48%;}</style></head>";
  html += "<body><h1>Radio Parrot</h1>";

  // Status
  html += "<div class='status ";
  html += (WiFi.status() == WL_CONNECTED) ? "connected'>Connected to: " + wifiSSID : "disconnected'>Not connected (AP Mode)";
  html += "</div>";

  // WiFi form
  html += "<h2>WiFi Settings</h2>";
  html += "<form action='/save' method='POST'>";
  html += "<label>SSID:</label><input name='ssid' value='" + wifiSSID + "'>";
  html += "<label>Password:</label><input name='pass' type='password' placeholder='Enter new password'>";

  // Weather location
  html += "<h2>Weather Location</h2>";
  html += "<div class='coords'>";
  html += "<input name='lat' id='lat' type='text' placeholder='Latitude' value='" + String(weatherLat, 4) + "'>";
  html += "<input name='lon' id='lon' type='text' placeholder='Longitude' value='" + String(weatherLon, 4) + "'>";
  html += "</div>";
  html += "<button type='button' class='btn' onclick='detectLocation()'>Detect My Location</button>";
  html += "<div id='locStatus'></div>";

  // Radio settings
  html += "<h2>Radio Settings</h2>";
  html += "<label>Frequency (MHz):</label><input name='freq' value='" + radioFreq + "' placeholder='451.0000'>";
  html += "<label>TX CTCSS (0000=none):</label><input name='txctcss' value='" + radioTxCTCSS + "' placeholder='0000'>";
  html += "<label>RX CTCSS (0000=none):</label><input name='rxctcss' value='" + radioRxCTCSS + "' placeholder='0000'>";
  html += "<label>Squelch (0-8):</label><input name='squelch' type='number' min='0' max='8' value='" + String(radioSquelch) + "'>";

  // Audio settings
  html += "<h2>Audio Settings</h2>";
  html += "<label>Voice Volume (0-100%):</label><input name='samvol' type='number' min='0' max='100' value='" + String(samVolumePercent) + "'>";
  html += "<label>Tone Volume (0-100%):</label><input name='tonevol' type='number' min='0' max='100' value='" + String(toneVolumePercent) + "'>";

  // Pre/post messages
  html += "<h2>Message Wrapping</h2>";
  html += "<label>Pre-message (spoken before every transmission):</label>";
  html += "<textarea name='premsg' rows='2' style='width:100%'>" + preMessage + "</textarea>";
  html += "<label>Post-message (spoken after every transmission):</label>";
  html += "<textarea name='postmsg' rows='2' style='width:100%'>" + postMessage + "</textarea>";
  html += "<details><summary>Available macros</summary>";
  html += "<code>{time}</code> 24h time, ";
  html += "<code>{time12}</code> 12h time, ";
  html += "<code>{date}</code> date, ";
  html += "<code>{day}</code> weekday, ";
  html += "<code>{hour}</code> hour, ";
  html += "<code>{minute}</code> minute, ";
  html += "<code>{battery}</code> battery %, ";
  html += "<code>{voltage}</code> battery volts, ";
  html += "<code>{slot}</code> next slot #, ";
  html += "<code>{slots_used}</code> used slots, ";
  html += "<code>{slots_total}</code> total slots, ";
  html += "<code>{freq}</code> frequency, ";
  html += "<code>{uptime}</code> uptime, ";
  html += "<code>{ip}</code> IP address";
  html += "</details>";

  // DTMF # message
  html += "<h2>DTMF # Message</h2>";
  html += "<label>Text to speak on DTMF # (empty to disable):</label>";
  html += "<textarea name='hashmsg' rows='3' style='width:100%'>" + dtmfHashMessage + "</textarea>";

  // Time & timezone
  html += "<h2>Time &amp; Timezone</h2>";
  html += "<div id='deviceTime' style='padding:8px;background:#eee;margin:5px 0;font-family:monospace;'></div>";
  html += "<label>Timezone (POSIX TZ string):</label>";
  html += "<input name='tz' id='tzInput' value='" + timezonePosix + "' placeholder='EST5EDT,M3.2.0,M11.1.0' style='width:100%'>";
  html += "<button type='button' onclick='detectTZ()'>Detect From Browser</button>";
  html += "<span id='tzStatus'></span><br>";
  html += "<label>Set Time (local):</label>";
  html += "<input name='manualtime' id='manualTime' placeholder='2025-06-15 14:30:00' style='width:60%'>";
  html += "<button type='button' onclick='setBrowserTime()'>Use Browser Time</button>";

  // Testing mode
  html += "<h2>Mode</h2>";
  html += "<label><input type='checkbox' name='testmode' value='1'" + String(testingMode ? " checked" : "") + "> Testing Mode (PTT disabled)</label>";

  html += "<br><br><input type='submit' value='Save & Reboot'>";
  html += "</form>";

  // Link to pins page
  html += "<p><a href='/pins'>Configure Pins</a></p>";

  // JavaScript for location detection
  html += "<script>";
  html += "function detectLocation(){";
  html += "document.getElementById('locStatus').innerHTML='Detecting...';";
  html += "fetch('http://ip-api.com/json/?fields=lat,lon,city,country')";
  html += ".then(r=>r.json()).then(d=>{";
  html += "document.getElementById('lat').value=d.lat.toFixed(4);";
  html += "document.getElementById('lon').value=d.lon.toFixed(4);";
  html += "document.getElementById('locStatus').innerHTML='Found: '+d.city+', '+d.country;";
  html += "}).catch(e=>{document.getElementById('locStatus').innerHTML='Detection failed';});";
  html += "}";

  // Timezone detection (IANA -> POSIX mapping)
  html += "function detectTZ(){";
  html += "var iana=Intl.DateTimeFormat().resolvedOptions().timeZone;";
  html += "var m={'America/New_York':'EST5EDT,M3.2.0,M11.1.0',";
  html += "'America/Chicago':'CST6CDT,M3.2.0,M11.1.0',";
  html += "'America/Denver':'MST7MDT,M3.2.0,M11.1.0',";
  html += "'America/Los_Angeles':'PST8PDT,M3.2.0,M11.1.0',";
  html += "'America/Toronto':'EST5EDT,M3.2.0,M11.1.0',";
  html += "'America/Vancouver':'PST8PDT,M3.2.0,M11.1.0',";
  html += "'America/Edmonton':'MST7MDT,M3.2.0,M11.1.0',";
  html += "'America/Winnipeg':'CST6CDT,M3.2.0,M11.1.0',";
  html += "'America/Halifax':'AST4ADT,M3.2.0,M11.1.0',";
  html += "'America/St_Johns':'NST3:30NDT,M3.2.0/0:01,M11.1.0/0:01',";
  html += "'Europe/London':'GMT0BST,M3.5.0/1,M10.5.0',";
  html += "'Europe/Berlin':'CET-1CEST,M3.5.0,M10.5.0/3',";
  html += "'Europe/Paris':'CET-1CEST,M3.5.0,M10.5.0/3',";
  html += "'Australia/Sydney':'AEST-10AEDT,M10.1.0,M4.1.0/3',";
  html += "'Pacific/Auckland':'NZST-12NZDT,M9.5.0,M4.1.0/3',";
  html += "'Asia/Tokyo':'JST-9',";
  html += "'Asia/Shanghai':'CST-8',";
  html += "'Asia/Kolkata':'IST-5:30',";
  html += "'UTC':'UTC0'};";
  html += "var p=m[iana]||'';";
  html += "if(p){document.getElementById('tzInput').value=p;";
  html += "document.getElementById('tzStatus').innerHTML=' '+iana+' &rarr; '+p;}";
  html += "else{document.getElementById('tzStatus').innerHTML=' \"'+iana+'\" not mapped. Enter POSIX string manually.';}";
  html += "}";

  // Set time from browser
  html += "function setBrowserTime(){";
  html += "var d=new Date();";
  html += "var s=d.getFullYear()+'-'+('0'+(d.getMonth()+1)).slice(-2)+'-'+('0'+d.getDate()).slice(-2)+' '";
  html += "+('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2)+':'+('0'+d.getSeconds()).slice(-2);";
  html += "var tz=document.getElementById('tzInput').value;";
  html += "fetch('/settime',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},";
  html += "body:'time='+encodeURIComponent(s)+'&tz='+encodeURIComponent(tz)})";
  html += ".then(r=>r.json()).then(d=>{if(d.ok)document.getElementById('manualTime').value='Set: '+s;});";
  html += "}";

  // Live clock display
  html += "function updateClock(){";
  html += "fetch('/status').then(r=>r.json()).then(d=>{";
  html += "var s='Device: '+(d.time||'not set');";
  html += "if(d.rtc)s+=' | RTC: OK';else s+=' | RTC: not found';";
  html += "if(d.ntp)s+=' | NTP: synced';";
  html += "document.getElementById('deviceTime').innerHTML=s;";
  html += "}).catch(e=>{});}";
  html += "setInterval(updateClock,1000);updateClock();";

  html += "</script>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  String newSSID = server.arg("ssid");
  String newPass = server.arg("pass");
  String newLat = server.arg("lat");
  String newLon = server.arg("lon");
  String newFreq = server.arg("freq");
  String newTxCTCSS = server.arg("txctcss");
  String newRxCTCSS = server.arg("rxctcss");
  String newSquelch = server.arg("squelch");
  String newSamVol = server.arg("samvol");
  String newToneVol = server.arg("tonevol");
  bool newTestMode = server.hasArg("testmode");

  preferences.begin("parrot", false);

  if (newSSID.length() > 0) {
    preferences.putString("ssid", newSSID);
  }
  if (newPass.length() > 0) {
    preferences.putString("password", newPass);
  }
  if (newLat.length() > 0) {
    preferences.putFloat("lat", newLat.toFloat());
  }
  if (newLon.length() > 0) {
    preferences.putFloat("lon", newLon.toFloat());
  }
  if (newFreq.length() > 0) {
    preferences.putString("freq", newFreq);
  }
  if (newTxCTCSS.length() > 0) {
    preferences.putString("txctcss", newTxCTCSS);
  }
  if (newRxCTCSS.length() > 0) {
    preferences.putString("rxctcss", newRxCTCSS);
  }
  if (newSquelch.length() > 0) {
    preferences.putInt("squelch", newSquelch.toInt());
  }
  if (newSamVol.length() > 0) {
    preferences.putInt("samvol", constrain(newSamVol.toInt(), 0, 100));
  }
  if (newToneVol.length() > 0) {
    preferences.putInt("tonevol", constrain(newToneVol.toInt(), 0, 100));
  }
  preferences.putBool("testmode", newTestMode);
  preferences.putString("hashmsg", server.arg("hashmsg"));
  preferences.putString("premsg", server.arg("premsg"));
  preferences.putString("postmsg", server.arg("postmsg"));
  if (server.hasArg("tz")) {
    preferences.putString("tz", server.arg("tz"));
  }

  preferences.end();

  // Handle manual time set before reboot (write to DS3231 so it persists)
  String manualTime = server.arg("manualtime");
  if (manualTime.length() >= 19 && rtcFound) {
    // Apply new TZ first so mktime interprets correctly
    String newTZ = server.arg("tz");
    if (newTZ.length() > 0) { setenv("TZ", newTZ.c_str(), 1); tzset(); }
    struct tm t = {};
    t.tm_year = manualTime.substring(0, 4).toInt() - 1900;
    t.tm_mon  = manualTime.substring(5, 7).toInt() - 1;
    t.tm_mday = manualTime.substring(8, 10).toInt();
    t.tm_hour = manualTime.substring(11, 13).toInt();
    t.tm_min  = manualTime.substring(14, 16).toInt();
    t.tm_sec  = manualTime.substring(17, 19).toInt();
    t.tm_isdst = -1;
    time_t epoch = mktime(&t);
    struct tm utc;
    gmtime_r(&epoch, &utc);
    ds3231Write(utc);
    Serial.println("Manual time written to RTC");
  }

  String html = "<!DOCTYPE html><html><head><title>Saved</title>";
  html += "<meta http-equiv='refresh' content='3;url=/'></head>";
  html += "<body><h1>Settings Saved!</h1><p>Rebooting...</p></body></html>";
  server.send(200, "text/html", html);

  delay(1000);
  ESP.restart();
}

void handleStatus() {
  String json = "{";
  json += "\"wifi\":\"" + String((WiFi.status() == WL_CONNECTED) ? "connected" : "disconnected") + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"ssid\":\"" + wifiSSID + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"ap_mode\":" + String(apMode ? "true" : "false") + ",";
  struct tm t;
  if (getLocalTime(&t, 0)) {
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &t);
    json += "\"time\":\"" + String(timeBuf) + "\",";
  } else {
    json += "\"time\":\"not set\",";
  }
  json += "\"tz\":\"" + timezonePosix + "\",";
  json += "\"rtc\":" + String(rtcFound ? "true" : "false") + ",";
  json += "\"ntp\":" + String(ntpSynced ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleSetTime() {
  String timeStr = server.arg("time");
  String tzStr = server.arg("tz");
  if (tzStr.length() > 0) {
    timezonePosix = tzStr;
    applyTimezone();
    preferences.begin("parrot", false);
    preferences.putString("tz", tzStr);
    preferences.end();
  }
  if (timeStr.length() >= 19) {
    struct tm t = {};
    t.tm_year = timeStr.substring(0, 4).toInt() - 1900;
    t.tm_mon  = timeStr.substring(5, 7).toInt() - 1;
    t.tm_mday = timeStr.substring(8, 10).toInt();
    t.tm_hour = timeStr.substring(11, 13).toInt();
    t.tm_min  = timeStr.substring(14, 16).toInt();
    t.tm_sec  = timeStr.substring(17, 19).toInt();
    t.tm_isdst = -1;
    time_t epoch = mktime(&t);  // interprets as local per TZ env
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    if (rtcFound) {
      struct tm utc;
      gmtime_r(&epoch, &utc);
      ds3231Write(utc);
    }
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid time format\"}");
  }
}

void handlePins() {
  String html = "<!DOCTYPE html><html><head><title>Pin Configuration</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;margin:20px;max-width:400px;}";
  html += "input{margin:5px 0;padding:8px;width:80px;}</style></head>";
  html += "<body><h1>Pin Configuration</h1>";
  html += "<p><strong>Warning:</strong> Incorrect pin settings can prevent boot. Only change if you know your hardware.</p>";
  html += "<form action='/savepins' method='POST'>";

  html += "<h2>Control Pins</h2>";
  html += "<label>PTT Pin:</label><input name='ptt' type='number' value='" + String(pinPTT) + "'><br>";
  html += "<label>Power Down Pin:</label><input name='pd' type='number' value='" + String(pinPD) + "'><br>";
  html += "<label>Audio/Squelch Pin:</label><input name='audioon' type='number' value='" + String(pinAudioOn) + "'><br>";

  html += "<h2>I2S Pins</h2>";
  html += "<label>MCLK:</label><input name='mclk' type='number' value='" + String(pinI2S_MCLK) + "'><br>";
  html += "<label>BCLK:</label><input name='bclk' type='number' value='" + String(pinI2S_BCLK) + "'><br>";
  html += "<label>LRCLK:</label><input name='lrclk' type='number' value='" + String(pinI2S_LRCLK) + "'><br>";
  html += "<label>Data In:</label><input name='din' type='number' value='" + String(pinI2S_DIN) + "'><br>";
  html += "<label>Data Out:</label><input name='dout' type='number' value='" + String(pinI2S_DOUT) + "'><br>";

  html += "<h2>Battery Monitor</h2>";
  html += "<label>VBAT Pin (-1 to disable):</label><input name='vbat' type='number' min='-1' value='" + String(pinVBAT) + "'><br>";

  html += "<br><input type='submit' value='Save & Reboot'>";
  html += "</form>";
  html += "<p><a href='/'>Back to Main</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSavePins() {
  preferences.begin("parrot", false);

  if (server.hasArg("ptt")) preferences.putInt("pinPTT", server.arg("ptt").toInt());
  if (server.hasArg("pd")) preferences.putInt("pinPD", server.arg("pd").toInt());
  if (server.hasArg("audioon")) preferences.putInt("pinAudioOn", server.arg("audioon").toInt());
  if (server.hasArg("mclk")) preferences.putInt("pinMCLK", server.arg("mclk").toInt());
  if (server.hasArg("bclk")) preferences.putInt("pinBCLK", server.arg("bclk").toInt());
  if (server.hasArg("lrclk")) preferences.putInt("pinLRCLK", server.arg("lrclk").toInt());
  if (server.hasArg("din")) preferences.putInt("pinDIN", server.arg("din").toInt());
  if (server.hasArg("dout")) preferences.putInt("pinDOUT", server.arg("dout").toInt());
  if (server.hasArg("vbat")) preferences.putInt("pinVBAT", server.arg("vbat").toInt());

  preferences.end();

  String html = "<!DOCTYPE html><html><head><title>Saved</title>";
  html += "<meta http-equiv='refresh' content='3;url=/pins'></head>";
  html += "<body><h1>Pin Settings Saved!</h1><p>Rebooting...</p></body></html>";
  server.send(200, "text/html", html);

  delay(1000);
  ESP.restart();
}

void initWiFi() {
  // Load settings from Preferences
  preferences.begin("parrot", true);  // read-only
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  weatherLat = preferences.getFloat("lat", DEFAULT_LAT);
  weatherLon = preferences.getFloat("lon", DEFAULT_LON);
  radioFreq = preferences.getString("freq", "451.0000");
  radioTxCTCSS = preferences.getString("txctcss", "0000");
  radioRxCTCSS = preferences.getString("rxctcss", "0000");
  radioSquelch = preferences.getInt("squelch", 4);

  // Audio settings
  samVolumePercent = preferences.getInt("samvol", 25);
  toneVolumePercent = preferences.getInt("tonevol", 12);

  // Pin configuration
  pinPTT = preferences.getInt("pinPTT", 33);
  pinPD = preferences.getInt("pinPD", 13);
  pinAudioOn = preferences.getInt("pinAudioOn", 4);
  pinI2S_MCLK = preferences.getInt("pinMCLK", 0);
  pinI2S_BCLK = preferences.getInt("pinBCLK", 26);
  pinI2S_LRCLK = preferences.getInt("pinLRCLK", 27);
  pinI2S_DIN = preferences.getInt("pinDIN", 14);
  pinI2S_DOUT = preferences.getInt("pinDOUT", 25);
  pinVBAT = preferences.getInt("pinVBAT", VBAT_PIN);  // Default 35, -1 to disable

  // Testing mode (default ON for safety)
  testingMode = preferences.getBool("testmode", true);
  dtmfHashMessage = preferences.getString("hashmsg", "");
  preMessage = preferences.getString("premsg", "");
  postMessage = preferences.getString("postmsg", "");
  timezonePosix = preferences.getString("tz", "");

  preferences.end();

  Serial.printf("Loaded SSID: %s\n", wifiSSID.c_str());
  Serial.printf("Weather location: %.4f, %.4f\n", weatherLat, weatherLon);

  // If no SSID configured, go straight to AP mode
  if (wifiSSID.length() == 0) {
    Serial.println("No WiFi configured, starting AP mode...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    dnsServer.start(53, "*", WiFi.softAPIP());  // Captive portal DNS
    Serial.printf("AP started: %s (password: %s)\n", AP_SSID, AP_PASSWORD);
    Serial.printf("Connect and visit http://%s\n", WiFi.softAPIP().toString().c_str());
    apMode = true;
  } else {
    // Try to connect
    Serial.printf("WiFi connecting to %s...\n", wifiSSID.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

    // Wait up to 10 seconds for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi connected! IP: http://%s\n", WiFi.localIP().toString().c_str());
      apMode = false;
      wifiReadyTime = millis() + WIFI_SETTLE_MS;
    } else {
      // Connection failed, start AP mode
      Serial.println("WiFi connection failed, starting AP mode...");
      WiFi.mode(WIFI_AP);
      WiFi.softAP(AP_SSID, AP_PASSWORD);
      dnsServer.start(53, "*", WiFi.softAPIP());  // Captive portal DNS
      Serial.printf("AP started: %s (password: %s)\n", AP_SSID, AP_PASSWORD);
      Serial.printf("Connect and visit http://%s\n", WiFi.softAPIP().toString().c_str());
      apMode = true;
    }
  }

  // Minimize WiFi RF interference with radio
  WiFi.setTxPower(WIFI_POWER_MINUS_1dBm);
  WiFi.setSleep(true);  // Modem sleep between beacons
  Serial.println("WiFi TX power set to minimum, modem sleep enabled");

  // Start web server in either mode
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/pins", handlePins);
  server.on("/savepins", HTTP_POST, handleSavePins);
  server.on("/status", handleStatus);
  server.on("/settime", HTTP_POST, handleSetTime);

  // Captive portal - redirect all unknown URLs to root
  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.println("Web server started on port 80");
}
