#include <TinyGPS++.h>
#include <Bounce2.h>
#include <TimeLib.h>
#include <SolarCalculator.h>
#include <U8g2lib.h>

// states
#define S_ADJUST_DURATION 0
#define S_ADJUST_TIME 1
#define S_MANUAL_RUN 2
#define S_DIRECT_RUN 3
#define S_RESET 4
#define S_INIT 5
#define NUM_MODES 6
#define S_OVERFLOW 100


// button pin assignments.
#define BTN_UP D3
#define BTN_CTRL D1
#define BTN_DOWN D0

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(
  U8G2_R0,          // rotation
  U8X8_PIN_NONE     // reset pin (not used)
);

TinyGPSPlus gps;
Bounce buttons[3]; // bounce object for each button.

int app_state = S_INIT;
const int buttonPins[3] = {BTN_DOWN, BTN_CTRL, BTN_UP};  // our buttons.
boolean display_needs_update = false;
int run_duration = 10;
int utc_offset = -5; // default to summer time in San Antonio.
bool time_is_fixed = false;
bool loc_is_fixed = false;
int gps_hour = 0;
int gps_minute = 0;
int gps_second = 0;
int gps_year = 0;
int gps_month = 0;
int gps_day = 0;
long last_screen_update = 0;
long last_button = 0; // last time one of the buttons was pressed. 
long direct_run_start = 0;
double lat = 0.0;
double lon = 0.0;
int sats = 0;
bool sunset_trigger = false;
long last_sunset_run = 0;
long last_millis = 0;
char screen[7][22];  // 7 lines of 21 characters + null terminator
time_t last_run_time = NULL;

void setup() {

  u8g2.begin();
  
  // u8g2.setFont(u8g2_font_ncenB08_tr);
  // u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.setFont(u8g2_font_6x10_tf);
  clear_screen_buffer();

  strncpy(screen[2], "mode: ", 21);   // Copy up to 21 characters
  
  // These are the momentary buttons
  for (int i = 0; i < 3; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);  // Buttons wired to GND
    buttons[i].attach(buttonPins[i]);
    buttons[i].interval(25);  // 25ms debounce time
  }

  // This is the relay pin.
  pinMode(D2, OUTPUT);
  digitalWrite(D2, LOW);     // Relay off

  Serial.begin(115200);    // For USB serial monitor
  Serial1.begin(9600);     // For GPS, typical default baud rate
  delay(100);

  delay(1900); // give time for USB to start.
  last_millis = millis();
  Serial.println("Initialization finished.");

  // set up is done. let's blink the onboard LED a few times.
  flash_onboard_led(10);
  
}

void flash_onboard_led(int num) {
  pinMode(13, OUTPUT);
  while (num-- > 0) {
    digitalWrite(13, LOW);
    delay(150);
    digitalWrite(13, HIGH);
    delay(150);
  }
}

void clear_screen_buffer() {
  for (int i = 0; i < 7; ++i) {
    for (int j = 0; j < 21; ++j) {
      screen[i][j] = ' ';
    }
    screen[i][21] = '\0';  // Null-terminate each row
  }
}

void draw_screen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);  // Each char ~6px wide, 8px tall
  for (int row = 0; row < 7; ++row) {
    u8g2.drawStr(0, (row + 1) * 9, screen[row]);  // Y positions: 8, 16, ..., 56
  }
  u8g2.sendBuffer();
}

void consume_gps_data() {
  // read it all.
  while (Serial1.available() > 0) {
    gps.encode(Serial1.read());
  }

  sats = gps.satellites.value();

  if (gps.location.isUpdated()) {
    // Serial.println(gps.location.lng(), 6);
    lat = gps.location.lat();
    lon = gps.location.lng();
    loc_is_fixed = true;
  }

  if (gps.time.isUpdated()) {
    display_needs_update = !time_is_fixed;
    time_is_fixed = true;
    gps_hour = gps.time.hour();
    gps_minute = gps.time.minute();
    gps_second = gps.time.second();
    long extra_millis = gps.time.age();

    if (gps.date.isUpdated()) {
      gps_year = gps.date.year();
      gps_month = gps.date.month();
      gps_day = gps.date.day();
    }

    setTime(gps_hour, gps_minute, gps_second, gps_day, gps_month, gps_year);
  }
}

bool maybe_adjust_mode() {
  if (buttons[1].fell()) {
    // we need to change modes.
    last_button = millis();
    display_needs_update = true;
    app_state = (app_state + 1) % NUM_MODES;

    // force the motor off if not in direct mode.
    if (app_state != S_DIRECT_RUN && digitalRead(D2) == HIGH) {
      digitalWrite(D2, LOW);
      // clear out the screen.
      strncpy(screen[5], "                     ", 21);
    }
  }
}

void maybe_go_up_or_down() {
  if (buttons[0].fell() || buttons[2].fell()) {
    display_needs_update = true;
    last_button = millis();
    // wait to see if this is a double press.
    delay(150);
    int increment = 0 + 
      (buttons[2].read() == LOW ? 1 : 0) + // <-- up
      (buttons[0].read() == LOW ? -1 : 0); // <-- down
    bool not_already_running = digitalRead(D2) == LOW;
    
    if (app_state == S_ADJUST_DURATION) {
      run_duration = max(0, run_duration + increment);
      // Serial.print("Run duration set to "); Serial.print(run_duration); Serial.println(" seconds.");
    } else if (app_state == S_ADJUST_TIME) {
      int new_utc_offset = utc_offset + increment;
      if (new_utc_offset >= -12 && new_utc_offset <= 12) {
        utc_offset += increment;
        // Serial.print("UTC offset set to "); Serial.print(utc_offset); Serial.println(" hours.");
      }
    } else if (app_state == S_MANUAL_RUN && increment == 0 && run_duration > 0 && not_already_running) {
      Serial.println("Manual run starting");
      do_run(run_duration);
    } else if (app_state == S_DIRECT_RUN && increment != 0) {
      bool turn_on = not_already_running && increment > 0;
      bool turn_off = !not_already_running && increment < 0;
      // there are a few other states which basically mean "leave it alone". We ignore those.
      if (turn_on) {
        direct_run_start = last_button;
        digitalWrite(D2, HIGH);
        last_run_time = now() + (utc_offset * 3600);
      } else if (turn_off) {
        digitalWrite(D2, LOW);
        last_run_time = now() + (utc_offset * 3600);
      }
    } else if (app_state == S_RESET && increment == 0) {
      Serial.println("Resetting...system will restart in 5 seconds");
      for (int i = 0; i < 5; i++) {
        snprintf(
          screen[5], sizeof(screen[5]),
          "Reset in %d seconds",
          5-i
        );
        display_needs_update = true;
        maybe_update_display();
        delay(1000);
      }
      NVIC_SystemReset();
    }
  }
}

void update_buttons() {
  for (int i = 0; i < 3; i++) {
    buttons[i].update();
  }
}

void maybe_update_display() {
  // millis() overflows every 50 days.
  long right_now = millis();
  long millis_since = right_now - last_screen_update;

  // we don't want to update all the time.
  // if (millis_since > 60000 || right_now - last_button < 5000) {
  if (millis_since > 100) {
    display_needs_update = true; // force update every minute regardless of need.
  }

  if (display_needs_update) {
    display_needs_update = false;
    last_screen_update = millis();
    if (time_is_fixed) {
      time_t local_time = now() + (utc_offset * 3600);
      Serial.print(hour(local_time)); Serial.print(":"); Serial.print(minute(local_time)); Serial.print(":"); Serial.print(second(local_time));
      Serial.print("  ");
      Serial.print(year(local_time)); Serial.print("/"); Serial.print(month(local_time)); Serial.print("/"); Serial.print(day(local_time));
      Serial.print("   ");
      snprintf(
        screen[0], sizeof(screen[0]),
        "%02d:%02d:%02d %04d/%02d/%02d",
        hour(local_time), minute(local_time), second(local_time),
        year(local_time), month(local_time), day(local_time)
      );
    }

    if (last_run_time != NULL) {
      snprintf(
        screen[4], sizeof(screen[4]),
        "last ran %02d:%02d:%02d %04d/%02d/%02d",
        hour(last_run_time), minute(last_run_time), second(last_run_time),
        year(last_run_time), month(last_run_time), day(last_run_time)
      );
    }

    snprintf(
      screen[3], sizeof(screen[6]),
      "run duration :%d",
      run_duration
    );
    Serial.print("run: "); Serial.print(run_duration); 
    Serial.print(", tzd: "); Serial.print(utc_offset);

    Serial.print(", mode:");
    if (app_state == S_ADJUST_DURATION) {
      Serial.print(" dur");
      strncpy(screen[2]+6, "set duration", 21-6);
    } else if (app_state == S_ADJUST_TIME) {
      Serial.print(" utc");
      strncpy(screen[2]+6, "set utc offset", 21-6);
    } else if (app_state == S_INIT) {
      Serial.print(" none");
      strncpy(screen[2]+6, "None", 21-6);
    } else if (app_state == S_MANUAL_RUN) {
      Serial.print(" manual");
      strncpy(screen[2]+6, "manual run", 21-6);
    } else if (app_state == S_DIRECT_RUN) {
      Serial.print(" direct");
      bool already_running = digitalRead(D2) == HIGH;
      if (!already_running) {
        // blank out the line
        strncpy(screen[5], "                     ", 21);
      } else {
        // figure out how long we've been running.
        int run_duration = (long)(right_now - direct_run_start)/1000;
        snprintf(
          screen[5], sizeof(screen[5]),
          "running :%d sec",
          run_duration
        );
      }
      strncpy(screen[2]+6, "direct run", 21-6);
    } else if (app_state == S_RESET) {
      Serial.print(" reset");
      strncpy(screen[2]+6, "RESET", 21-6);
    } else if (app_state == S_OVERFLOW) {
      Serial.print(" overflow");
      strncpy(screen[2]+6, "OVERFLOW", 21-6);
    }

    if (loc_is_fixed && time_is_fixed) {
      // we don't care about the varies of whether we're dealing with sunset of today or tomorrow since
      // it's going to be within a few minutes of the correct time.
      // note that sunset will be computed in hours UTC.
      // what does 25.13 mean?
      double sunset_in = calc_next_sunset();
      Serial.print(", sunset_in="); Serial.print(sunset_in, 2);// Serial.print(", now="); Serial.print(now());
      Serial.print(", sats="); Serial.print(sats);
      snprintf(
        screen[1], sizeof(screen[1]),
        "sunset in %.2f hours",
        sunset_in
      );
      double uptime_days = (double)last_screen_update / 86400000.0;
      snprintf(
        screen[6], sizeof(screen[6]),
        "%02d sats UTC%+d   %.1fd",
        sats, utc_offset, uptime_days
      );
    }

    Serial.println("");
    draw_screen();
    // flash_onboard_led(2);
  }
}

double calc_next_sunset() {
  time_t utc = now();
  double utc_hour = (double)hour(utc);
  double utc_mins = (double)minute(utc);
  double transit, sunrise, sunset;
  calcSunriseSunset(utc, lat, lon, transit, sunrise, sunset);
  double sunset_in = sunset - utc_hour - (utc_mins/60.0);
  // fix for https://github.com/gdusbabek/pdoser/issues/1
  // we go from sunset in X hours (0 < X < 2) to sunset in X+24 hours.
  // So we never get to within 1 hour of sunset. Fix that by modulating by 24.
  while (sunset_in > 24.001) {
    sunset_in -= 24.0;
  }
  return sunset_in;
}


// given: run_for > 0;
void do_run(int run_for) {
  digitalWrite(D2, HIGH);
  last_run_time = now() + (utc_offset * 3600);
  while (run_for > 0) {
    Serial.print("   Will run for "); Serial.print(run_for); Serial.println(" seconds.");
    snprintf(
      screen[5], sizeof(screen[5]),
      "Run for %d seconds",
      run_for
    );
    display_needs_update = true;
    maybe_update_display();
    delay(1000);
    run_for -= 1;
  }
  digitalWrite(D2, LOW);
  snprintf(
    screen[5], sizeof(screen[5]),
    "                  "
  );
  display_needs_update = true;
  maybe_update_display();
}

void maybe_do_automatic_run() {
  // I think there is a subtle bug here when millis() overflows.
  // We're going to address that by just restarting when we detect overflow...

  bool waited_long_enough = (millis() - last_sunset_run) > (1000 * 3600 * 3); // 3 hours.
  bool not_already_running = digitalRead(D2) == LOW;
  bool sunset_soon = calc_next_sunset() < 0.1; // within 6 mins of sunset.
  bool gtz_duration = run_duration > 0;

  // we only do an automatic run if conditaions are right:
  // we know what time it is AND we know where we are AND...
  // we haven't run in the last three hours AND we're not currently running AND...
  // the sun will be setting within 6 minutes AND run time is more than zero
  if (time_is_fixed && loc_is_fixed && waited_long_enough && not_already_running && sunset_soon && gtz_duration) {
    Serial.println("Doing automatic run");
    do_run(run_duration);
    last_sunset_run = millis();
  }
}

void maybe_restart() {
  long next_millis = millis();
  if (next_millis < last_millis) {
    // this happens every 50 days. the millis counter as overflowed.
    Serial.println("Millis counter has overflowed. Restarting in 10s");
    app_state = S_OVERFLOW;
    for (int i = 0; i < 10; i++) {
      snprintf(
        screen[5], sizeof(screen[5]),
        "Oflow reset in %d sec",
        5-i
      );
      display_needs_update = true;
      maybe_update_display();
      delay(1000);
    }
    NVIC_SystemReset();
  } else {
    last_millis = next_millis;
  }
}

void loop() {
  maybe_restart();
  consume_gps_data();
  update_buttons();
  maybe_adjust_mode();
  maybe_go_up_or_down();
  maybe_update_display();
  maybe_do_automatic_run();
  delay(10);
}
