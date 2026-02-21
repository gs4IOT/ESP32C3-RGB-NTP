// timezones.h — Timezone options for WiFiManager portal dropdown
// Add or remove entries as needed — value must be a valid IANA timezone string
// Full list: https://en.wikipedia.org/wiki/List_of_tz_database_time_zones

#ifndef TIMEZONES_H
#define TIMEZONES_H

const char TZ_OPTIONS_HTML[] =
  "<br/><label>Timezone</label>"
  "<select name='tz'>"
  "<option value='America/Edmonton'>America/Edmonton (MST/MDT)</option>"
  "<option value='America/Denver'>America/Denver (MST/MDT)</option>"
  "<option value='America/Phoenix'>America/Phoenix (MST no DST)</option>"
  "<option value='America/New_York'>America/New_York (EST)</option>"
  "<option value='America/Chicago'>America/Chicago (CST)</option>"
  "<option value='America/Los_Angeles'>America/Los_Angeles (PST)</option>"
  "<option value='America/Anchorage'>America/Anchorage (AKST)</option>"
  "<option value='Pacific/Honolulu'>Pacific/Honolulu (HST)</option>"
  "<option value='Europe/London'>Europe/London (GMT)</option>"
  "<option value='Europe/Paris'>Europe/Paris (CET)</option>"
  "<option value='Europe/Berlin'>Europe/Berlin (CET)</option>"
  "<option value='Asia/Dubai'>Asia/Dubai (GST)</option>"
  "<option value='Asia/Kolkata'>Asia/Kolkata (IST)</option>"
  "<option value='Asia/Tokyo'>Asia/Tokyo (JST)</option>"
  "<option value='Australia/Sydney'>Australia/Sydney (AEDT)</option>"
  "<option value='Pacific/Auckland'>Pacific/Auckland (NZST)</option>"
  "</select>";

#endif
