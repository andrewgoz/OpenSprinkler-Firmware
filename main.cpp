/* OpenSprinkler Unified Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Main loop
 * Feb 2015 @ OpenSprinkler.com
 *
 * This file is part of the OpenSprinkler Firmware
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <limits.h>

#include "types.h"
#include "OpenSprinkler.h"
#include "program.h"
#include "weather.h"
#include "opensprinkler_server.h"
#include "mqtt.h"
#include "main.h"
#include "notifier.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#if defined(ARDUINO)
	#if defined(ESP8266)
		ESP8266WebServer *update_server = NULL;
		DNSServer *dns = NULL;
		ENC28J60lwIP enc28j60(PIN_ETHER_CS); // ENC28J60 lwip for wired Ether
		Wiznet5500lwIP w5500(PIN_ETHER_CS); // W5500 lwip for wired Ether
		lwipEth eth;
		bool useEth = false; // tracks whether we are using WiFi or wired Ether connection
	#else
		EthernetServer *m_server = NULL;
		EthernetClient *m_client = NULL;
		SdFat sd;	// SD card object
		bool useEth = true;
	#endif
	unsigned long getNtpTime();
#else // header and defs for RPI/Linux
	bool useEth = false;
#endif

#if defined(USE_OTF)
	OTF::OpenThingsFramework *otf = NULL;
#endif

#if defined(USE_SSD1306)
	#if defined(ESP8266)
	static uint16_t led_blink_ms = LED_FAST_BLINK;
	#else
	static uint16_t led_blink_ms = 0;
	#endif
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

const char *user_agent_string = "OpenSprinkler/" TOSTRING(OS_FW_VERSION) "#" TOSTRING(OS_FW_MINOR);

void manual_start_program(unsigned char, unsigned char);

// Small variations have been added to the timing values below
// to minimize conflicting events
#define NTP_SYNC_INTERVAL       86413L  // NTP sync interval (in seconds)
#define CHECK_NETWORK_INTERVAL  601     // Network checking timeout (in seconds)
#define CHECK_WEATHER_TIMEOUT   21613L  // Weather check interval (in seconds)
#define CHECK_WEATHER_SUCCESS_TIMEOUT 86400L // Weather check success interval (in seconds)
#define LCD_BACKLIGHT_TIMEOUT     15    // LCD backlight timeout (in seconds))
#define PING_TIMEOUT              200   // Ping test timeout (in ms)
#define UI_STATE_MACHINE_INTERVAL 50    // how often does ui_state_machine run (in ms)
#define CLIENT_READ_TIMEOUT       5     // client read timeout (in seconds)
#define DHCP_CHECKLEASE_INTERVAL  3600L // DHCP check lease interval (in seconds)
// Define buffers: need them to be sufficiently large to cover string option reading
char ether_buffer[ETHER_BUFFER_SIZE*2]; // ethernet buffer, make it twice as large to allow overflow
char tmp_buffer[TMP_BUFFER_SIZE*2]; // scratch buffer, make it twice as large to allow overflow

// ====== Object defines ======
OpenSprinkler os; // OpenSprinkler object
ProgramData pd;   // ProgramdData object
NotifQueue notif; // NotifQueue object

/* ====== Robert Hillman (RAH)'s implementation of flow sensor ======
 * flow_begin - time when valve turns on
 * flow_start - time when flow starts being measured (i.e. 2 mins after flow_begin approx
 * flow_stop - time when valve turns off (last rising edge pulse detected before off)
 * flow_gallons - total # of gallons+1 from flow_start to flow_stop
 * flow_last_gpm - last flow rate measured (averaged over flow_gallons) from last valve stopped (used to write to log file). */
ulong flow_begin, flow_start, flow_stop, flow_gallons, flow_rt_reset, last_flow_rt;
ulong flow_count = 0;
unsigned char prev_flow_state = HIGH;
float flow_last_gpm = 0;
int32_t flow_rt_period = -1;
uint32_t reboot_timer = 0;

void flow_poll() {
	ulong curr = millis();

	// Resets counter if timeout occurs
	if (flow_rt_reset && curr > flow_rt_reset) {
		os.flowcount_rt = 0;
		flow_rt_period = -1;
		flow_rt_reset = 0;
	}

	if (flow_rt_period < 0) {
		last_flow_rt = curr;
	}

	#if defined(ESP8266)
	if(os.hw_rev>=2) {
		pinMode(PIN_SENSOR1, INPUT); // Work-around for PIN_SENSOR1 on OS3.2 and above
		pinMode(PIN_SENSOR1, INPUT_PULLUP);
	}
	#endif


	unsigned char curr_flow_state = digitalReadExt(PIN_SENSOR1);
	if((!prev_flow_state) || curr_flow_state) { // only record on falling edge
		prev_flow_state = curr_flow_state;
		return;
	}
	prev_flow_state = curr_flow_state;
	flow_count++;

	/* RAH implementation of flow sensor */
	if (flow_start == 0) { 
		flow_gallons = 0;
		flow_start = curr;
	} // if first pulse, record time

	if ((curr-flow_start)<90000) {
		flow_gallons=0;
	} // wait 90 seconds before recording flow_begin
	else {
		if (flow_gallons==1) {
			flow_begin = curr;
		}
	}

	// Use exponential moving average (alpha=0.2) if flow has been previosuly calculated, otherwise just set the value
	ulong curr_period = curr - last_flow_rt;
	if (flow_rt_period > 0) {
		flow_rt_period = (curr_period  / 5 + flow_rt_period * 4 / 5);
	} else {
		flow_rt_period = curr_period;
	}

	// calculates the flow rate scaled by the window size to simulated a fixed point number
	if (flow_rt_period > 0) {
		os.flowcount_rt = (ulong) (FLOWCOUNT_RT_WINDOW * 1000L / flow_rt_period);
		// Sets the timeout to be 10x the last period
		flow_rt_reset = curr + (curr - last_flow_rt) * 10;
	} else {
		os.flowcount_rt = 0;
		flow_rt_reset = 0;
	}

	last_flow_rt = curr;

	flow_stop = curr; // get time in ms for stop
	flow_gallons++;  // increment gallon count for each poll
	/* End of RAH implementation of flow sensor */
}

#if defined(USE_DISPLAY)
// ====== UI defines ======
static char ui_anim_chars[3] = {'.', 'o', 'O'};

#define UI_STATE_DEFAULT   0
#define UI_STATE_DISP_IP   1
#define UI_STATE_DISP_GW   2
#define UI_STATE_RUNPROG   3

static unsigned char ui_state = UI_STATE_DEFAULT;
static unsigned char ui_state_runprog = 0;

bool ui_confirm(PGM_P str) {
	os.lcd_print_line_clear_pgm(str, 0);
	os.lcd_print_line_clear_pgm(PSTR("(B1:No, B3:Yes)"), 1);
	unsigned char button;
	ulong start = millis();
	do {
		button = os.button_read(BUTTON_WAIT_NONE);
		if((button&BUTTON_MASK)==BUTTON_3 && (button&BUTTON_FLAG_DOWN)) return true;
		if((button&BUTTON_MASK)==BUTTON_1 && (button&BUTTON_FLAG_DOWN)) return false;
		delay(10);
	} while(millis() - start < 2500);
	return false;
}

void ui_state_machine() {
	// to avoid ui_state_machine taking too much computation time
	// we run it only every UI_STATE_MACHINE_INTERVAL ms
	static uint32_t last_usm = 0;
	if(millis() - last_usm <= UI_STATE_MACHINE_INTERVAL) { return; }
	last_usm = millis();

#if defined(USE_SSD1306)
	// process screen led
	static ulong led_toggle_timeout = 0;
	if(led_blink_ms) {
		if(millis()>led_toggle_timeout) {
			os.toggle_screen_led();
			led_toggle_timeout = millis() + led_blink_ms;
		}
	}
#endif

	if (!os.button_timeout) {
		os.lcd_set_brightness(0);
		ui_state = UI_STATE_DEFAULT;  // also recover to default state
	}

	// read button, if something is pressed, wait till release
	unsigned char button = os.button_read(BUTTON_WAIT_HOLD);

	if (button & BUTTON_FLAG_DOWN) {  // repond only to button down events
		os.button_timeout = LCD_BACKLIGHT_TIMEOUT;
		os.lcd_set_brightness(1);
	} else {
		return;
	}

	switch(ui_state) {
	case UI_STATE_DEFAULT:
		switch (button & BUTTON_MASK) {
		case BUTTON_1:
			if (button & BUTTON_FLAG_HOLD) {  // holding B1
				if (digitalReadExt(PIN_BUTTON_3)==0) { // if B3 is pressed while holding B1, run a short test (internal test)
					if(!ui_confirm(PSTR("Start 2s test?"))) {ui_state = UI_STATE_DEFAULT; break;}
					manual_start_program(255, 0);
				} else if (digitalReadExt(PIN_BUTTON_2)==0) { // if B2 is pressed while holding B1, display gateway IP
					#if defined(USE_SSD1306)
						os.lcd.setAutoDisplay(false);
					#endif
					os.lcd.clear(0, 1);
					os.lcd.setCursor(0, 0);
					#if defined(ARDUINO)
					#if defined(ESP8266)
					if (useEth) { os.lcd.print(eth.gatewayIP()); }
					else { os.lcd.print(WiFi.gatewayIP()); }
					#else
					{ os.lcd.print(Ethernet.gatewayIP()); }
					#endif
					#else
					route_t route = get_route();
					char str[INET_ADDRSTRLEN];

					inet_ntop(AF_INET, &(route.gateway), str, INET_ADDRSTRLEN);
					os.lcd.print(str);
					#endif
					os.lcd.setCursor(0, 1);
					os.lcd_print_pgm(PSTR("(gwip)"));
					ui_state = UI_STATE_DISP_IP;
					#if defined(USE_SSD1306)
						os.lcd.display();
						os.lcd.setAutoDisplay(true);
					#endif
				} else {  // if no other button is clicked, stop all zones
					if(!ui_confirm(PSTR("Stop all zones?"))) {ui_state = UI_STATE_DEFAULT; break;}
					reset_all_stations();
				}
			} else {  // clicking B1: display device IP and port
				#if defined(USE_SSD1306)
					os.lcd.setAutoDisplay(false);
				#endif
				os.lcd.clear(0, 1);
				os.lcd.setCursor(0, 0);
				#if defined(ARDUINO)
				#if defined(ESP8266)
				if (useEth) { os.lcd.print(eth.localIP()); }
				else { os.lcd.print(WiFi.localIP()); }
				#else
				{ os.lcd.print(Ethernet.localIP()); }
				#endif
				#else
				route_t route = get_route();
				char str[INET_ADDRSTRLEN];
				in_addr_t ip = get_ip_address(route.iface);

				inet_ntop(AF_INET, &ip, str, INET_ADDRSTRLEN);
				os.lcd.print(str);
				#endif
				os.lcd.setCursor(0, 1);
				os.lcd_print_pgm(PSTR(":"));
				uint16_t httpport = (uint16_t)(os.iopts[IOPT_HTTPPORT_1]<<8) + (uint16_t)os.iopts[IOPT_HTTPPORT_0];
				os.lcd.print(httpport);
				os.lcd_print_pgm(PSTR(" (ip:port)"));
				#if defined(USE_OTF)
					os.lcd.setCursor(0, 2);
					os.lcd_print_pgm(PSTR("OTC:"));
					switch(otf->getCloudStatus()) {
						case OTF::NOT_ENABLED:
							os.lcd_print_pgm(PSTR(" not enabled"));
							break;
						case OTF::UNABLE_TO_CONNECT:
							os.lcd_print_pgm(PSTR("connecting.."));
							break;
						case OTF::DISCONNECTED:
							os.lcd_print_pgm(PSTR("disconnected"));
							break;
						case OTF::CONNECTED:
							os.lcd_print_pgm(PSTR(" Connected"));
							break;
					}
				#endif
				
				ui_state = UI_STATE_DISP_IP;
				#if defined(USE_SSD1306)
					os.lcd.display();
					os.lcd.setAutoDisplay(true);
				#endif
			}
			break;
		case BUTTON_2:
			if (button & BUTTON_FLAG_HOLD) {  // holding B2
				if (digitalReadExt(PIN_BUTTON_1)==0) { // if B1 is pressed while holding B2, display external IP
					os.lcd_print_ip((unsigned char*)(&os.nvdata.external_ip), 1);
					os.lcd.setCursor(0, 1);
					os.lcd_print_pgm(PSTR("(eip)"));
					ui_state = UI_STATE_DISP_IP;
				} else if (digitalReadExt(PIN_BUTTON_3)==0) {  // if B3 is pressed while holding B2, display last successful weather call
					//os.lcd.clear(0, 1);
					os.lcd_print_time(os.checkwt_success_lasttime);
					os.lcd.setCursor(0, 1);
					os.lcd_print_pgm(PSTR("(lswc)"));
					ui_state = UI_STATE_DISP_IP;
				} else {  // if no other button is clicked, reboot
					if(!ui_confirm(PSTR("Reboot device?"))) {ui_state = UI_STATE_DEFAULT; break;}
					os.reboot_dev(REBOOT_CAUSE_BUTTON);
				}
			} else {  // clicking B2: display MAC
				os.lcd.clear(0, 1);
				unsigned char mac[6];
				os.load_hardware_mac(mac, useEth);
				os.lcd_print_mac(mac);
				ui_state = UI_STATE_DISP_GW;
			}
			break;
		case BUTTON_3:
			if (button & BUTTON_FLAG_HOLD) {  // holding B3
				if (digitalReadExt(PIN_BUTTON_1)==0) {  // if B1 is pressed while holding B3, display up time
					os.lcd_print_time(os.powerup_lasttime);
					os.lcd.setCursor(0, 1);
					os.lcd_print_pgm(PSTR("(lupt) cause:"));
					os.lcd.print(os.last_reboot_cause);
					ui_state = UI_STATE_DISP_IP;
				} else if(digitalReadExt(PIN_BUTTON_2)==0) {  // if B2 is pressed while holding B3, reset to AP and reboot
					#if defined(ESP8266)
					if(!ui_confirm(PSTR("Reset to AP?"))) {ui_state = UI_STATE_DEFAULT; break;}
					os.reset_to_ap();
					#endif
				} else {  // if no other button is clicked, go to Run Program main menu
					os.lcd_print_line_clear_pgm(PSTR("Run a Program:"), 0);
					os.lcd_print_line_clear_pgm(PSTR("Click B3 to list"), 1);
					ui_state = UI_STATE_RUNPROG;
				}
			} else {  // clicking B3: switch board display (cycle through master and all extension boards)
				os.status.display_board = (os.status.display_board + 1) % (os.nboards);
			}
			break;
		}
		break;
	case UI_STATE_DISP_IP:
	case UI_STATE_DISP_GW:
		ui_state = UI_STATE_DEFAULT;
		break;
	case UI_STATE_RUNPROG:
		if ((button & BUTTON_MASK)==BUTTON_3) {
			if (button & BUTTON_FLAG_HOLD) {
				// start
				manual_start_program(ui_state_runprog, 0);
				ui_state = UI_STATE_DEFAULT;
			} else {
				ui_state_runprog = (ui_state_runprog+1) % (pd.nprograms+1);
				os.lcd_print_line_clear_pgm(PSTR("Hold B3 to start"), 0);
				if(ui_state_runprog > 0) {
					ProgramStruct prog;
					pd.read(ui_state_runprog-1, &prog);
					os.lcd_print_line_clear_pgm(PSTR(" "), 1);
					os.lcd.setCursor(0, 1);
					os.lcd.print((int)ui_state_runprog);
					os.lcd_print_pgm(PSTR(". "));
					os.lcd.print(prog.name);
				} else {
					os.lcd_print_line_clear_pgm(PSTR("0. Test (1 min)"), 1);
				}
			}
		}
		break;
	}
}
#endif


// ======================
// Setup Function
// ======================
#if defined(ARDUINO)
void do_setup() {
	/* Clear WDT reset flag. */
#if defined(ESP8266)
	WiFi.persistent(false);
	led_blink_ms = LED_FAST_BLINK;
#else
	MCUSR &= ~(1<<WDRF);
#endif

	DEBUG_BEGIN(115200);
	DEBUG_PRINTLN(F("started"));

	os.begin();          // OpenSprinkler init
	os.options_setup();  // Setup options

	pd.init();           // ProgramData init

	// set time using RTC if it exists
	if(RTC.exists())	setTime(RTC.get());
	os.lcd_print_time(os.now_tz());  // display time to LCD
	os.powerup_lasttime = os.now_tz();

#if defined(OS_AVR)
	// enable WDT
	/* In order to change WDE or the prescaler, we need to
	 * set WDCE (This will allow updates for 4 clock cycles).
	 */
	WDTCSR |= (1<<WDCE) | (1<<WDE);
	/* set new watchdog timeout prescaler value */
	WDTCSR = 1<<WDP3 | 1<<WDP0;  // 8.0 seconds
	/* Enable the WD interrupt (note no reset). */
	WDTCSR |= _BV(WDIE);
#endif
	if (os.start_network()) {  // initialize network
		os.status.network_fails = 0;
	} else {
		os.status.network_fails = 1;
	}

	os.status.req_network = 0;
	os.status.req_ntpsync = 1;

	os.mqtt.init();
	os.status.req_mqtt_restart = true;

	os.apply_all_station_bits(); // reset station bits

	// because at reboot we don't know if special stations
	// are in OFF state, here we explicitly turn them off
	for(unsigned char sid=0;sid<os.nstations;sid++) {
		os.switch_special_station(sid, 0);
	}

	os.button_timeout = LCD_BACKLIGHT_TIMEOUT;
}

// Arduino software reset function
void(* sysReset) (void) = 0;

#if defined(OS_AVR)
volatile unsigned char wdt_timeout = 0;
/** WDT interrupt service routine */
ISR(WDT_vect)
{
	wdt_timeout += 1;
	// this isr is called every 8 seconds
	if (wdt_timeout > 15) {
		// reset after 120 seconds of timeout
		sysReset();
	}
}
#endif

#else
void initialize_otf();

void do_setup() {
	initialiseEpoch();   // initialize time reference for millis() and micros()
	os.begin();          // OpenSprinkler init
	os.options_setup();  // Setup options

	pd.init();           // ProgramData init

	if (os.start_network()) {  // initialize network
		DEBUG_PRINTLN("network established.");
		os.status.network_fails = 0;
	} else {
		DEBUG_PRINTLN("network failed.");
		os.status.network_fails = 1;
	}
	os.status.req_network = 0;

	// because at reboot we don't know if special stations
	// are in OFF state, here we explicitly turn them off
	for(unsigned char sid=0;sid<os.nstations;sid++) {
		os.switch_special_station(sid, 0);
	}

	os.mqtt.init();
	os.status.req_mqtt_restart = true;

	initialize_otf();
}

#endif

void turn_on_station(unsigned char sid, ulong duration);
static void check_network();
void check_weather();
static bool process_special_program_command(const char*, uint32_t curr_time);
static void perform_ntp_sync();

#if defined(ESP8266)
bool delete_log_oldest();
void start_server_ap();
void start_server_client();
static Ticker reboot_ticker;

void reboot_in(uint32_t ms) {
	if(os.state != OS_STATE_WAIT_REBOOT) {
		os.state = OS_STATE_WAIT_REBOOT;
		DEBUG_PRINTLN(F("Prepare to restart..."));
		reboot_ticker.once_ms(ms, ESP.restart);
	}
}
#else
void handle_web_request(char *p);
#endif


/** Main Loop */
void do_loop()
{
	static ulong flowpoll_timeout=0;
	if(os.iopts[IOPT_SENSOR1_TYPE]==SENSOR_TYPE_FLOW) {
	// handle flow sensor using polling every 1ms (maximum freq 1/(2*1ms)=500Hz)
		ulong curr = millis();
		if(curr!=flowpoll_timeout) {
			flowpoll_timeout = curr;
			flow_poll();
		}
	}


	static time_os_t last_time = 0;
	static ulong last_minute = 0;

	unsigned char bid, sid, s, pid, qid, gid, bitvalue;
	ProgramStruct prog;

	os.status.mas = os.iopts[IOPT_MASTER_STATION];
	os.status.mas2= os.iopts[IOPT_MASTER_STATION_2];
	time_os_t curr_time = os.now_tz();

	// ====== Process Ethernet packets ======
#if defined(ARDUINO)	// Process Ethernet packets for Arduino
	#if defined(ESP8266)
	static ulong connecting_timeout;
	switch(os.state) {
	case OS_STATE_INITIAL:
		if(useEth) {
			led_blink_ms = 0;
			os.set_screen_led(LOW);
			os.lcd.clear();
			os.save_wifi_ip();
			start_server_client();
			os.state = OS_STATE_CONNECTED;
			connecting_timeout = 0;
		} else if(os.get_wifi_mode()==WIFI_MODE_AP) {
			start_server_ap();
			dns->setErrorReplyCode(DNSReplyCode::NoError);
			dns->start(53, "*", WiFi.softAPIP());
			os.state = OS_STATE_CONNECTED;
			connecting_timeout = 0;
		} else {
			led_blink_ms = LED_SLOW_BLINK;
			if(os.sopt_load(SOPT_STA_BSSID_CHL).length()>0 && os.wifi_channel<255) {
				start_network_sta(os.wifi_ssid.c_str(), os.wifi_pass.c_str(), (int32_t)os.wifi_channel, os.wifi_bssid);
			}
			else
				start_network_sta(os.wifi_ssid.c_str(), os.wifi_pass.c_str());
			os.config_ip();
			os.state = OS_STATE_CONNECTING;
			connecting_timeout = millis() + 120000L;
			os.lcd.setCursor(0, -1);
			os.lcd.print(F("Connecting to..."));
			os.lcd.setCursor(0, 2);
			os.lcd.print(os.wifi_ssid);
		}
		break;

	case OS_STATE_TRY_CONNECT:
		led_blink_ms = LED_SLOW_BLINK;
		if(os.sopt_load(SOPT_STA_BSSID_CHL).length()>0 && os.wifi_channel<255) {
			start_network_sta_with_ap(os.wifi_ssid.c_str(), os.wifi_pass.c_str(), (int32_t)os.wifi_channel, os.wifi_bssid);
		}
		else
			start_network_sta_with_ap(os.wifi_ssid.c_str(), os.wifi_pass.c_str());
		os.config_ip();
		os.state = OS_STATE_CONNECTED;
		break;

	case OS_STATE_CONNECTING:
		if(WiFi.status() == WL_CONNECTED) {
			led_blink_ms = 0;
			os.set_screen_led(LOW);
			os.lcd.clear();
			os.save_wifi_ip();
			start_server_client();
			os.state = OS_STATE_CONNECTED;
			connecting_timeout = 0;
		} else {
			if(millis()>connecting_timeout) {
				os.state = OS_STATE_INITIAL;
				WiFi.disconnect(true);
				DEBUG_PRINTLN(F("timeout"));
			}
		}
		break;

	case OS_STATE_WAIT_REBOOT:
		if(dns) dns->processNextRequest();
		if(otf) otf->loop();
		if(update_server) update_server->handleClient();
		break;

	case OS_STATE_CONNECTED:
		if(os.get_wifi_mode() == WIFI_MODE_AP) {
			dns->processNextRequest();
			update_server->handleClient();
			otf->loop();
			connecting_timeout = 0;
			if(os.get_wifi_mode()==WIFI_MODE_STA) {
				// already in STA mode, waiting to reboot
				break;
			}
			if(WiFi.status()==WL_CONNECTED && WiFi.localIP() && reboot_timer!=0) {
				DEBUG_PRINTLN(F("STA connected, set up reboot timer"));
				reboot_timer = os.now_tz() + 10;
				//os.reboot_dev(REBOOT_CAUSE_WIFIDONE);
			}
		} else {
			if(useEth || WiFi.status() == WL_CONNECTED) {
				update_server->handleClient();
				otf->loop();
				connecting_timeout = 0;
			} else {
				// todo: better handling of WiFi disconnection
				DEBUG_PRINTLN(F("WiFi disconnected, going back to initial"));
				os.state = OS_STATE_INITIAL;
				WiFi.disconnect(true);
			}
		}
		break;
	}

	#else // AVR

	static unsigned long dhcp_timeout = 0;
	if(curr_time > dhcp_timeout) {
		Ethernet.maintain();
		dhcp_timeout = curr_time + DHCP_CHECKLEASE_INTERVAL;
	}
	EthernetClient client = m_server->available();
	if (client) {
		ulong cli_timeout = now() + CLIENT_READ_TIMEOUT;
		size_t size = 0;
		while(client.connected() && now() < cli_timeout) {
			size = client.available();	// wait till we have client data available
			if(size>0) break;
		}
		if(size>0) {
			size_t len = 0;
			while (client.available() && now()<cli_timeout) {
				size_t read = client.readBytesUntil('\n', ether_buffer+len, min((int) (ETHER_BUFFER_SIZE - len - 1), ETHER_BUFFER_SIZE));
				char rc = ether_buffer[len];
				len += read;
				ether_buffer[len++] = '\n';
				if(read==1 && rc=='\r') { break; }
			}
			if(len>0) {
				m_client = &client;
				ether_buffer[len] = 0;  // properly end the buffer
				handle_web_request(ether_buffer);
				m_client = NULL;
			}
		}
		client.stop();
	}

	wdt_reset();  // reset watchdog timer
	wdt_timeout = 0;

	#endif

	ui_state_machine();

#else // Process Ethernet packets for RPI/LINUX
	if(otf) otf->loop();
#if defined(USE_DISPLAY)
	ui_state_machine();
#endif
#endif	// Process Ethernet packets

	// Start up MQTT when we have a network connection
	if (os.status.req_mqtt_restart && os.network_connected()) {
		DEBUG_PRINTLN(F("req_mqtt_restart"));
		os.mqtt.begin();
		os.status.req_mqtt_restart = false;
		os.mqtt.subscribe();
	}
	os.mqtt.loop();

	// The main control loop runs once every second
	if (curr_time != last_time) {

		#if defined(ESP8266)
		if(os.hw_rev>=2) {
			pinMode(PIN_SENSOR1, INPUT_PULLUP); // this seems necessary for OS 3.2
			pinMode(PIN_SENSOR2, INPUT_PULLUP);
		}
		#endif

		last_time = curr_time;
		if (os.button_timeout) os.button_timeout--;

#if defined(USE_DISPLAY)
		if (!ui_state)
			os.lcd_print_time(curr_time);  // print time
#endif

		// ====== Check raindelay status ======
		if (os.status.rain_delayed) {
			if (curr_time >= os.nvdata.rd_stop_time) {  // rain delay is over
				os.raindelay_stop();
			}
		} else {
			if (os.nvdata.rd_stop_time > curr_time) {  // rain delay starts now
				os.raindelay_start();
			}
		}

		// ====== Check controller status changes and write log ======
		if (os.old_status.rain_delayed != os.status.rain_delayed) {
			if (os.status.rain_delayed) {
				// rain delay started, record time
				os.raindelay_on_lasttime = curr_time;
				notif.add(NOTIFY_RAINDELAY, LOGDATA_RAINDELAY, 1);

			} else {
				// rain delay stopped, write log
				write_log(LOGDATA_RAINDELAY, curr_time);
				notif.add(NOTIFY_RAINDELAY, LOGDATA_RAINDELAY, 0);
			}
			os.old_status.rain_delayed = os.status.rain_delayed;
		}

		// ====== Check binary (i.e. rain or soil) sensor status ======
		os.detect_binarysensor_status(curr_time);

		if(os.old_status.sensor1_active != os.status.sensor1_active) {
			// send notification when sensor1 becomes active
			if(os.status.sensor1_active) {
				os.sensor1_active_lasttime = curr_time;
				notif.add(NOTIFY_SENSOR1, LOGDATA_SENSOR1, 1);
			} else {
				write_log(LOGDATA_SENSOR1, curr_time);
				notif.add(NOTIFY_SENSOR1, LOGDATA_SENSOR1, 0);
			}
		}
		os.old_status.sensor1_active = os.status.sensor1_active;

		if(os.old_status.sensor2_active != os.status.sensor2_active) {
			// send notification when sensor1 becomes active
			if(os.status.sensor2_active) {
				os.sensor2_active_lasttime = curr_time;
				notif.add(NOTIFY_SENSOR2, LOGDATA_SENSOR2, 1);
			} else {
				write_log(LOGDATA_SENSOR2, curr_time);
				notif.add(NOTIFY_SENSOR2, LOGDATA_SENSOR2, 0);
			}
		}
		os.old_status.sensor2_active = os.status.sensor2_active;

		// ===== Check program switch status =====
		unsigned char pswitch = os.detect_programswitch_status(curr_time);
		if(pswitch > 0) {
			reset_all_stations_immediate(); // immediately stop all stations
		}
		if (pswitch & 0x01) {
			if(pd.nprograms > 0)	manual_start_program(1, 0);
		}
		if (pswitch & 0x02) {
			if(pd.nprograms > 1)	manual_start_program(2, 0);
		}

		// ====== Schedule program data ======
		ulong curr_minute = curr_time / 60;
		boolean match_found = false;
		RuntimeQueueStruct *q;
		// since the granularity of start time is minute
		// we only need to check once every minute
		if (curr_minute != last_minute) {
			last_minute = curr_minute;

			apply_monthly_adjustment(curr_time); // check and apply monthly adjustment here, if it's selected

			// check through all programs
			for(pid=0; pid<pd.nprograms; pid++) {
				pd.read(pid, &prog);	// todo future: reduce load time
				bool will_delete = false;
				unsigned char runcount = prog.check_match(curr_time, &will_delete);
				if(runcount>0) {
					// program match found
					// check and process special program command
					if(process_special_program_command(prog.name, curr_time))	continue;

					// get station ordering
					unsigned char order[os.nstations];
					prog.gen_station_runorder(runcount, order);
					// process all selected stations
					for(unsigned char oi=0;oi<os.nstations;oi++) {
						sid=order[oi];
						bid=sid>>3;
						s=sid&0x07;
						// skip if the station is a master station (because master cannot be scheduled independently
						if ((os.status.mas==sid+1) || (os.status.mas2==sid+1))
							continue;

						// if station has non-zero water time and the station is not disabled
						if (prog.durations[sid] && !(os.attrib_dis[bid]&(1<<s))) {
							// water time is scaled by watering percentage
							ulong water_time = water_time_resolve(prog.durations[sid]);
							// if the program is set to use weather scaling
							if (prog.use_weather) {
								unsigned char wl = os.iopts[IOPT_WATER_PERCENTAGE];
								water_time = water_time * wl / 100;
								if (wl < 20 && water_time < 10) // if water_percentage is less than 20% and water_time is less than 10 seconds
																								// do not water
									water_time = 0;
							}

							if (water_time) {
								// check if water time is still valid
								// because it may end up being zero after scaling
								q = pd.enqueue();
								if (q) {
									q->st = 0;
									q->dur = water_time;
									q->sid = sid;
									q->pid = pid+1;
									match_found = true;
								} else {
									// queue is full
								}
							}// if water_time
						}// if prog.durations[sid]
					}// for sid
					if(match_found) {
						notif.add(NOTIFY_PROGRAM_SCHED, pid, prog.use_weather?os.iopts[IOPT_WATER_PERCENTAGE]:100);
					}
					//delete run-once if on final runtime (stations have already been queued)
					if(will_delete){
						pd.del(pid);
					}
				}// if check_match
			}// for pid

			// calculate start and end time
			if (match_found) {
				schedule_all_stations(curr_time);

				// For debugging: print out queued elements
				/*DEBUG_PRINT("en:");
				for(q=pd.queue;q<pd.queue+pd.nqueue;q++) {
					DEBUG_PRINT("[");
					DEBUG_PRINT(q->sid);
					DEBUG_PRINT(",");
					DEBUG_PRINT(q->dur);
					DEBUG_PRINT(",");
					DEBUG_PRINT(q->st);
					DEBUG_PRINT("]");
				}
				DEBUG_PRINTLN("");*/
			}
		}//if_check_current_minute

		// ====== Run program data ======
		// Check if a program is running currently
		// If so, do station run-time keeping
		if (os.status.program_busy){
			// first, go through run time queue to assign queue elements to stations
			q = pd.queue;
			qid=0;
			for(;q<pd.queue+pd.nqueue;q++,qid++) {
				sid=q->sid;
				unsigned char sqi=pd.station_qid[sid];
				// skip if station is already assigned a queue element
				// and that queue element has an earlier start time
				if(sqi<255 && pd.queue[sqi].st<q->st) continue;
				// otherwise assign the queue element to station
				pd.station_qid[sid]=qid;
			}
			// next, go through the stations and perform time keeping
			for(bid=0;bid<os.nboards; bid++) {
				bitvalue = os.station_bits[bid];
				for(s=0;s<8;s++) {
					unsigned char sid = bid*8+s;

					// skip master stations and any station that's not in the queue
					if (os.status.mas == sid+1) continue;
					if (os.status.mas2== sid+1) continue;
					if (pd.station_qid[sid]==255) continue;

					q = pd.queue + pd.station_qid[sid];

					// if current station is not running, check if we should turn it on
					if(!((bitvalue >> s) & 1)) {
						if (curr_time >= q->st && curr_time < q->st+q->dur) {
							turn_on_station(sid, q->st+q->dur-curr_time); // the last parameter is expected run time
						} //if curr_time > scheduled_start_time
					} // if current station is not running

					// check if this station should be turned off
					if (q->st > 0) {
						if (curr_time >= q->st+q->dur) {
							turn_off_station(sid, curr_time);
						}
					}
				}//end_s
			}//end_bid

			// finally, go through the queue again and clear up elements marked for removal
			int qi;
			for(qi=pd.nqueue-1;qi>=0;qi--) {
				q=pd.queue+qi;
				if(!q->dur || curr_time >= q->deque_time) {
					pd.dequeue(qi);
				}
			}

			// process dynamic events
			process_dynamic_events(curr_time);

			// activate / deactivate valves
			os.apply_all_station_bits();

			// check through runtime queue, calculate the last stop time of sequential stations
			memset(pd.last_seq_stop_times, 0, sizeof(ulong)*NUM_SEQ_GROUPS);
			time_os_t sst;
			unsigned char re=os.iopts[IOPT_REMOTE_EXT_MODE];
			q = pd.queue;
			for(;q<pd.queue+pd.nqueue;q++) {
				sid = q->sid;
				bid = sid>>3;
				s = sid&0x07;
				gid = os.get_station_gid(sid);
				// check if any sequential station has a valid stop time
				// and the stop time must be larger than curr_time
				sst = q->st + q->dur;
				if (sst>curr_time) {
					// only need to update last_seq_stop_time for sequential stations
					if (os.is_sequential_station(sid) && !re) {
						pd.last_seq_stop_times[gid] = (sst > pd.last_seq_stop_times[gid]) ? sst : pd.last_seq_stop_times[gid];
					}
				}
			}

			// if the runtime queue is empty
			// reset all stations
			if (!pd.nqueue) {
				// turn off all stations
				os.clear_all_station_bits();
				os.apply_all_station_bits();
				pd.reset_runtime(); // reset runtime
				os.status.program_busy = 0; // reset program busy bit
				pd.clear_pause(); // TODO: what if pause hasn't expired and a new program is scheduled to run?

				// log flow sensor reading if flow sensor is used
				if(os.iopts[IOPT_SENSOR1_TYPE]==SENSOR_TYPE_FLOW) {
					write_log(LOGDATA_FLOWSENSE, curr_time);
					notif.add(NOTIFY_FLOWSENSOR, (flow_count>os.flowcount_log_start)?(flow_count-os.flowcount_log_start):0);
				}

				// in case some options have changed while executing the program
				os.status.mas = os.iopts[IOPT_MASTER_STATION]; // update master station
				os.status.mas2= os.iopts[IOPT_MASTER_STATION_2]; // update master2 station
			}
		}//if_some_program_is_running

		// handle master
		for (unsigned char mas = MASTER_1; mas < NUM_MASTER_ZONES; mas++) {

			unsigned char mas_id = os.masters[mas][MASOPT_SID];

			if (mas_id) { // if this master station is set
				int16_t mas_on_adj = os.get_on_adj(mas);
				int16_t mas_off_adj = os.get_off_adj(mas);

				unsigned char masbit = 0;

				for(sid = 0; sid < os.nstations; sid++) {
					// skip if this is the master station
					if (mas_id == sid + 1) continue;

					if(pd.station_qid[sid]==255) continue; // skip if station is not in the queue

					q = pd.queue + pd.station_qid[sid];

					if (os.bound_to_master(q->sid, mas)) {
						// check if timing is within the acceptable range
						if (curr_time >= q->st + mas_on_adj &&
							curr_time <= q->st + q->dur + mas_off_adj) {
							masbit = 1;
							break;
						}
					}
				}

				os.set_station_bit(mas_id - 1, masbit);
			}
		}

		if (os.status.pause_state) {
			if (os.pause_timer > 0) {
				os.pause_timer--;
			} else {
				os.clear_all_station_bits();
				pd.clear_pause();
			}
		}
		// process dynamic events
		process_dynamic_events(curr_time);

		// handle master on / off notif events
		for (unsigned char mas = MASTER_1; mas < NUM_MASTER_ZONES; mas++) {
			unsigned char mas_id = os.masters[mas][MASOPT_SID];
			if (mas_id) { // if this master station is defined
				time_os_t laston = os.masters_last_on[mas];
				unsigned char masbit = os.get_station_bit(mas_id - 1);
				if(!laston && masbit) { // master is about to turn on
					notif.add(NOTIFY_STATION_ON, mas_id - 1, 0);
					os.masters_last_on[mas] = curr_time;
				}
				if(laston > 0 && !masbit) { // master is about to turn off
					notif.add(NOTIFY_STATION_OFF, mas_id - 1, (curr_time>laston) ? (curr_time-laston) : 0);
					os.masters_last_on[mas] = 0;
				}
			}
		}

		// activate/deactivate valves
		os.apply_all_station_bits();

#if defined(USE_DISPLAY)
		// process LCD display
		if (!ui_state) { os.lcd_print_screen(ui_anim_chars[(unsigned long)curr_time%3]); }
#endif

		// handle reboot request
		// check safe_reboot condition
		if (os.status.safe_reboot && (curr_time > reboot_timer)) {
			// if no program is running at the moment
			if (!os.status.program_busy) {
				// and if no program is scheduled to run in the next minute
				bool willrun = false;
				bool will_delete = false;
				for(pid=0; pid<pd.nprograms; pid++) {
					pd.read(pid, &prog);
					if(prog.check_match(curr_time+60, &will_delete)) {
						willrun = true;
						break;
					}
				}
				if (!willrun) {
					os.reboot_dev(os.nvdata.reboot_cause);
				}
			}
		} else if(reboot_timer && (curr_time > reboot_timer)) {
			os.reboot_dev(REBOOT_CAUSE_TIMER);
		}

		// perform ntp sync
		// instead of using curr_time, which may change due to NTP sync itself
		// we use Arduino's millis() method
		if (curr_time % NTP_SYNC_INTERVAL == 0) os.status.req_ntpsync = 1;
		//if((millis()/1000) % NTP_SYNC_INTERVAL==15) os.status.req_ntpsync = 1;
		perform_ntp_sync();

		// check network connection
		if (curr_time && (curr_time % CHECK_NETWORK_INTERVAL==0))  os.status.req_network = 1;
		check_network();

		// check weather
		check_weather();

		// process notifier events
		if(os.network_connected()) {
			notif.run();
		}

		if(os.weather_update_flag & WEATHER_UPDATE_WL) {
			// at the moment, we only send notification if water level changed
			// the other changes, such as sunrise, sunset changes are ignored for notification
			notif.add(NOTIFY_WEATHER_UPDATE, 0, os.iopts[IOPT_WATER_PERCENTAGE]);
			os.weather_update_flag = 0;
		}
		static unsigned char reboot_notification = 1;
		if(reboot_notification) {
			reboot_notification = 0;
			notif.add(NOTIFY_REBOOT);
		}
	}

	#if !defined(ARDUINO)
		delay(1); // For OSPI/LINUX, sleep 1 ms to minimize CPU usage
	#endif
}

/** Check and process special program command */
static bool process_special_program_command(const char* pname, uint32_t curr_time) {
	if(pname[0]==':') {	// special command start with :
		if(strncmp(pname, ":>reboot_now", 12) == 0) {
			os.status.safe_reboot = 0; // reboot regardless of program status
			reboot_timer = curr_time + 65; // set a timer to reboot in 65 seconds
			// this is to avoid the same command being executed again right after reboot
			return true;
		} else if(strncmp(pname, ":>reboot", 8) == 0) {
			os.status.safe_reboot = 1; // by default reboot should only happen when controller is idle
			reboot_timer = curr_time + 65; // set a timer to reboot in 65 seconds
			// this is to avoid the same command being executed again right after reboot
			return true;
		}
	}
	return false;
}

/** Make weather query */
void check_weather() {
	// do not check weather if
	// - network check has failed, or
	// - the controller is in remote extension mode
	if (os.status.network_fails>0 || os.iopts[IOPT_REMOTE_EXT_MODE]) return;
	if (os.status.program_busy) return;

	if (!os.network_connected()) return;

	time_os_t ntz = os.now_tz();
	if (os.checkwt_success_lasttime && (ntz > os.checkwt_success_lasttime + CHECK_WEATHER_SUCCESS_TIMEOUT)) {
		// if last successful weather call timestamp is more than allowed threshold
		// and if the selected adjustment method is not one of the manual methods
		// reset watering percentage to 100
		// todo: the firmware currently needs to be explicitly aware of which adjustment methods, this is not ideal
		os.checkwt_success_lasttime = 0;
		unsigned char method = os.iopts[IOPT_USE_WEATHER];
		if(!(method==WEATHER_METHOD_MANUAL || method==WEATHER_METHOD_AUTORAINDELY || method==WEATHER_METHOD_MONTHLY)) {
			os.iopts[IOPT_WATER_PERCENTAGE] = 100; // reset watering percentage to 100%
			wt_rawData[0] = 0; 		// reset wt_rawData and errCode
			wt_errCode = HTTP_RQT_NOT_RECEIVED;
		}
	} else if (!os.checkwt_lasttime || (ntz > os.checkwt_lasttime + CHECK_WEATHER_TIMEOUT)) {
		os.checkwt_lasttime = ntz;
		#if defined(ARDUINO)
		if (!ui_state) {
			os.lcd_print_line_clear_pgm(PSTR("Check Weather..."),1);
		}
		#endif
		GetWeather();
	}
}

/** Turn on a station
 * This function turns on a scheduled station
 */
void turn_on_station(unsigned char sid, ulong duration) {
	// RAH implementation of flow sensor
	flow_start=0;
	//Added flow_gallons reset to station turn on.
	flow_gallons=0;  

	if (os.set_station_bit(sid, 1, duration)) {
		notif.add(NOTIFY_STATION_ON, sid, duration);
	}
}

// after removing element q, update remaining stations in its group
void handle_shift_remaining_stations(RuntimeQueueStruct* q, unsigned char gid, time_os_t curr_time) {
	RuntimeQueueStruct *s = pd.queue;
	time_os_t q_end_time = q->st + q->dur;
	ulong remainder = 0;

	if (q_end_time > curr_time) { // remainder is non-zero
		remainder = (q->st < curr_time) ? q_end_time - curr_time : q->dur;
		for ( ; s < pd.queue + pd.nqueue; s++) {

			// ignore station to be removed and stations in other groups
			if (s == q || os.get_station_gid(s->sid) != gid || !os.is_sequential_station(s->sid)) {
				continue;
			}

			// only shift stations following current station
			if (s->st >= q_end_time) {
				s->st -= remainder;
				s->deque_time -= remainder;
			}
		}
	}
	pd.last_seq_stop_times[gid] -= remainder;
	pd.last_seq_stop_times[gid] += 1;
}

/** Turn off a station
 * This function turns off a scheduled station
 * writes a log record and determines if
 * the station should be removed from the queue
 */
void turn_off_station(unsigned char sid, time_os_t curr_time, unsigned char shift) {

	unsigned char qid = pd.station_qid[sid];
	// ignore request if trying to turn off a zone that's not even in the queue
	if (qid >= pd.nqueue)  {
		return;
	}
	RuntimeQueueStruct *q = pd.queue + qid;
	unsigned char force_dequeue = 0;
	unsigned char station_bit = os.is_running(sid);
	unsigned char gid = os.get_station_gid(q->sid);

	if (shift && os.is_sequential_station(sid) && !os.iopts[IOPT_REMOTE_EXT_MODE]) {
		handle_shift_remaining_stations(q, gid, curr_time);
	}

	if (curr_time >= q->deque_time) {
		if (station_bit) {
			force_dequeue = 1;
		} else { // if already off just remove from the queue
			pd.dequeue(qid);
			pd.station_qid[sid] = 0xFF;
			return;
		}
	} else if (curr_time >= q->st + q->dur) { // end time and dequeue time are not equal due to master handling
		if (!station_bit) { return; }
	} //else { return; }

	os.set_station_bit(sid, 0);

	// RAH implementation of flow sensor
	if (flow_gallons > 1) {
		if(flow_stop <= flow_begin) flow_last_gpm = 0;
		else flow_last_gpm = (float) 60000 / (float)((flow_stop-flow_begin) / (flow_gallons - 1));
	}// RAH calculate GPM, 1 pulse per gallon
	else {flow_last_gpm = 0;}  // RAH if not one gallon (two pulses) measured then record 0 gpm

	// check if the current time is past the scheduled start time,
	// because we may be turning off a station that hasn't started yet
	if (curr_time >= q->st) {
		// record lastrun log (only for non-master stations)
		if (os.status.mas != (sid + 1) && os.status.mas2 != (sid + 1)) {
			pd.lastrun.station = sid;
			pd.lastrun.program = q->pid;
			pd.lastrun.duration = curr_time - q->st;
			pd.lastrun.endtime = curr_time;

			// log station run
			write_log(LOGDATA_STATION, curr_time); // LOG_TODO
			notif.add(NOTIFY_STATION_OFF, sid, pd.lastrun.duration);
			notif.add(NOTIFY_FLOW_ALERT, sid, pd.lastrun.duration);
		}
	}

	// make necessary adjustments to sequential time stamps
	int16_t station_delay = water_time_decode_signed(os.iopts[IOPT_STATION_DELAY_TIME]);
	if (q->st + q->dur + station_delay == pd.last_seq_stop_times[gid]) { // if removing last station in group
		pd.last_seq_stop_times[gid] = 0;
	}

	if (force_dequeue) {
		pd.dequeue(qid);
		pd.station_qid[sid] = 0xFF;
	}
}

/** Process dynamic events
 * such as rain delay, rain sensing
 * and turn off stations accordingly
 */
void process_dynamic_events(time_os_t curr_time) {
	// check if rain is detected
	bool sn1 = false;
	bool sn2 = false;
	bool rd  = os.status.rain_delayed;
	bool en = os.status.enabled;

	if((os.iopts[IOPT_SENSOR1_TYPE] == SENSOR_TYPE_RAIN || os.iopts[IOPT_SENSOR1_TYPE] == SENSOR_TYPE_SOIL)
		 && os.status.sensor1_active)
		sn1 = true;

	if((os.iopts[IOPT_SENSOR2_TYPE] == SENSOR_TYPE_RAIN || os.iopts[IOPT_SENSOR2_TYPE] == SENSOR_TYPE_SOIL)
		 && os.status.sensor2_active)
		sn2 = true;

	unsigned char sid, s, bid, qid, igs, igs2, igrd;
	for(bid=0;bid<os.nboards;bid++) {
		igs = os.attrib_igs[bid];
		igs2= os.attrib_igs2[bid];
		igrd= os.attrib_igrd[bid];

		for(s=0;s<8;s++) {
			sid=bid*8+s;

			// ignore master stations because they are handled separately
			if (os.status.mas == sid+1) continue;
			if (os.status.mas2== sid+1) continue;
			// If this is a normal program (not a run-once or test program)
			// and either the controller is disabled, or
			// if raining and ignore rain bit is cleared
			// FIX ME
			qid = pd.station_qid[sid];
			if(qid==255) continue;
			RuntimeQueueStruct *q = pd.queue + qid;

			if(q->pid>=99) continue;  // if this is a manually started program, proceed
			if(!en)	{q->deque_time=curr_time; turn_off_station(sid, curr_time);}  // if system is disabled, turn off zone
			if(rd && !(igrd&(1<<s))) {q->deque_time=curr_time; turn_off_station(sid, curr_time);}  // if rain delay is on and zone does not ignore rain delay, turn it off
			if(sn1&& !(igs &(1<<s))) {q->deque_time=curr_time; turn_off_station(sid, curr_time);}  // if sensor1 is on and zone does not ignore sensor1, turn it off
			if(sn2&& !(igs2&(1<<s))) {q->deque_time=curr_time; turn_off_station(sid, curr_time);}  // if sensor2 is on and zone does not ignore sensor2, turn it off
		}
	}
}

/* Scheduler
 * this function determines the appropriate start and dequeue times
 * of stations bound to master stations with on and off adjustments
 */
void handle_master_adjustments(time_os_t curr_time, RuntimeQueueStruct *q, unsigned char gid, ulong *seq_start_times) {

	int16_t start_adj = 0;
	int16_t dequeue_adj = 0;

	for (unsigned char mas = MASTER_1; mas < NUM_MASTER_ZONES; mas++) {

		unsigned char masid = os.masters[mas][MASOPT_SID];

		if (masid && os.bound_to_master(q->sid, mas)) {

			int16_t mas_on_adj = os.get_on_adj(mas);
			int16_t mas_off_adj = os.get_off_adj(mas);

			start_adj = min(start_adj, mas_on_adj);
			dequeue_adj = max(dequeue_adj, mas_off_adj);
		}
	}

	// in case of negative master on adjustment
	// push back station's start time to allow sufficient time to turn on master
	if (q->st - curr_time < abs(start_adj)) {
		q->st += abs(start_adj);
		seq_start_times[gid] += abs(start_adj);
	}

	q->deque_time = q->st + q->dur + dequeue_adj;
}

/** Scheduler
 * This function loops through the queue
 * and schedules the start time of each station
 */
void schedule_all_stations(time_os_t curr_time) {
	ulong con_start_time = curr_time + 1;   // concurrent start time
	// if the queue is paused, make sure the start time is after the scheduled pause ends
	if (os.status.pause_state) {
		con_start_time += os.pause_timer;
	}
	int16_t station_delay = water_time_decode_signed(os.iopts[IOPT_STATION_DELAY_TIME]);
	ulong seq_start_times[NUM_SEQ_GROUPS];  // sequential start times
	for(unsigned char i=0;i<NUM_SEQ_GROUPS;i++) {
		seq_start_times[i] = con_start_time;
		// if the sequential queue already has stations running
		if (pd.last_seq_stop_times[i] > curr_time) {
			seq_start_times[i] = pd.last_seq_stop_times[i] + station_delay;
		}
	}
	RuntimeQueueStruct *q = pd.queue;
	unsigned char re = os.iopts[IOPT_REMOTE_EXT_MODE];
	unsigned char gid;

	// go through runtime queue and calculate start time of each station
	for(;q<pd.queue+pd.nqueue;q++) {
		if(q->st) continue; // if this queue element has already been scheduled, skip
		if(!q->dur) continue; // if the element has been marked to reset, skip
		gid = os.get_station_gid(q->sid);

		// use sequential scheduling per sequential group
		// apply station delay time
		if (os.is_sequential_station(q->sid) && !re) {
			q->st = seq_start_times[gid];
			seq_start_times[gid] += q->dur;
			seq_start_times[gid] += station_delay; // add station delay time
		} else {
			// otherwise, concurrent scheduling
			q->st = con_start_time;
			// stagger concurrent stations by 1 second
			con_start_time++;
		}

		handle_master_adjustments(curr_time, q, gid, seq_start_times);

		if (!os.status.program_busy) {
			os.status.program_busy = 1;  // set program busy bit
			// start flow count
			if(os.iopts[IOPT_SENSOR1_TYPE] == SENSOR_TYPE_FLOW) {  // if flow sensor is connected
				os.flowcount_log_start = flow_count;
				os.sensor1_active_lasttime = curr_time;
			}
		}
	}
}

/** Immediately reset all stations
 * No log records will be written
 */
void reset_all_stations_immediate() {
	os.clear_all_station_bits();
	os.apply_all_station_bits();
	pd.reset_runtime();
	pd.clear_pause();
}

/** Reset all stations
 * This function sets the duration of
 * every station to 0, which causes
 * all stations to turn off in the next processing cycle.
 * Stations will be logged
 */
void reset_all_stations() {
	RuntimeQueueStruct *q = pd.queue;
	// go through runtime queue and assign water time to 0
	for(;q<pd.queue+pd.nqueue;q++) {
		q->dur = 0;
	}
}


/** Manually start a program
 * If pid==0, this is a test program (1 minute per station)
 * If pid==255, this is a short test program (2 second per station)
 * If pid > 0. run program pid-1
 */
void manual_start_program(unsigned char pid, unsigned char uwt) {
	boolean match_found = false;
	reset_all_stations_immediate();
	ProgramStruct prog;
	ulong dur;
	unsigned char sid, bid, s;
	if ((pid>0)&&(pid<255)) {
		pd.read(pid-1, &prog);
		notif.add(NOTIFY_PROGRAM_SCHED, pid-1, uwt?os.iopts[IOPT_WATER_PERCENTAGE]:100, 1);
	}
	for(sid=0;sid<os.nstations;sid++) {
		bid=sid>>3;
		s=sid&0x07;
		// skip if the station is a master station (because master cannot be scheduled independently
		if ((os.status.mas==sid+1) || (os.status.mas2==sid+1))
			continue;
		dur = 60;
		if(pid==255)  dur=2;
		else if(pid>0)
			dur = water_time_resolve(prog.durations[sid]);
		if(uwt) {
			dur = dur * os.iopts[IOPT_WATER_PERCENTAGE] / 100;
		}
		if(dur>0 && !(os.attrib_dis[bid]&(1<<s))) {
			RuntimeQueueStruct *q = pd.enqueue();
			if (q) {
				q->st = 0;
				q->dur = dur;
				q->sid = sid;
				q->pid = 254;
				match_found = true;
			}
		}
	}
	if(match_found) {
		schedule_all_stations(os.now_tz());
	}
}


// ================================
// ====== LOGGING FUNCTIONS =======
// ================================
#if defined(ARDUINO)
char LOG_PREFIX[] = "/logs/";
#else
char LOG_PREFIX[] = "./logs/";
#endif

/** Generate log file name
 * Log files will be named /logs/xxxxx.txt
 */
void make_logfile_name(char *name) {
#if defined(ARDUINO)
	#if !defined(ESP8266)
	sd.chdir("/");
	#endif
#endif
	strcpy(tmp_buffer+TMP_BUFFER_SIZE-10, name); // hack: we do this because name is from tmp_buffer too
	strcpy(tmp_buffer, LOG_PREFIX);
	strcat(tmp_buffer, tmp_buffer+TMP_BUFFER_SIZE-10);
	strcat_P(tmp_buffer, PSTR(".txt"));
}

/* To save RAM space, we store log type names
 * in program memory, and each name
 * must be strictly two characters with an ending 0
 * so each name is 3 characters total
 */
static const char log_type_names[] PROGMEM =
	"  \0"
	"s1\0"
	"rd\0"
	"wl\0"
	"fl\0"
	"s2\0"
	"cu\0";

/** write run record to log on SD card */
void write_log(unsigned char type, time_os_t curr_time) {

	if (!os.iopts[IOPT_ENABLE_LOGGING]) return;

	// file name will be logs/xxxxx.tx where xxxxx is the day in epoch time
	snprintf (tmp_buffer, TMP_BUFFER_SIZE, "%lu", curr_time / 86400);
	make_logfile_name(tmp_buffer);

	// Step 1: open file if exists, or create new otherwise,
	// and move file pointer to the end
#if defined(ARDUINO) // prepare log folder for Arduino

	#if defined(ESP8266)
	File file = LittleFS.open(tmp_buffer, "r+");
	if(!file) {
		FSInfo fs_info;
		LittleFS.info(fs_info);
		// check if we are getting close to run out of space, and delete some oldest files
		if(fs_info.totalBytes < fs_info.usedBytes + fs_info.blockSize * 4) {
			// delete the oldest 7 files (1 week of log)
			for(unsigned char i=0;i<7;i++)	delete_log_oldest();
		}
		file = LittleFS.open(tmp_buffer, "w");
		if(!file) return;
	}
	file.seek(0, SeekEnd);
	#else
	sd.chdir("/");
	if (sd.chdir(LOG_PREFIX) == false) {
		// create dir if it doesn't exist yet
		if (sd.mkdir(LOG_PREFIX) == false) {
			return;
		}
	}
	SdFile file;
	int ret = file.open(tmp_buffer, O_CREAT | O_WRITE );
	file.seekEnd();
	if(!ret) {
		return;
	}
	#endif

#else // prepare log folder for RPI/LINUX
	struct stat st;
	if(stat(get_filename_fullpath(LOG_PREFIX), &st)) {
		if(mkdir(get_filename_fullpath(LOG_PREFIX), S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH | S_IWOTH | S_IXOTH)) {
			return;
		}
	}
	FILE *file;
	file = fopen(get_filename_fullpath(tmp_buffer), "rb+");
	if(!file) {
		file = fopen(get_filename_fullpath(tmp_buffer), "wb");
		if (!file)	return;
	}
	fseek(file, 0, SEEK_END);
#endif	// prepare log folder

	// Step 2: prepare data buffer
	strcpy_P(tmp_buffer, PSTR("["));

	if(type == LOGDATA_STATION) {
		size_t size = strlen(tmp_buffer);
		snprintf(tmp_buffer + size, TMP_BUFFER_SIZE - size , "%d", pd.lastrun.program);
		strcat_P(tmp_buffer, PSTR(","));
		size = strlen(tmp_buffer);
		snprintf(tmp_buffer + size, TMP_BUFFER_SIZE - size , "%d", pd.lastrun.station);
		strcat_P(tmp_buffer, PSTR(","));
		// duration is unsigned integer
		size = strlen(tmp_buffer);
		snprintf(tmp_buffer + size, TMP_BUFFER_SIZE - size , "%lu", (ulong)pd.lastrun.duration);

	} else {
		ulong lvalue=0;
		if(type==LOGDATA_FLOWSENSE) {
			lvalue = (flow_count>os.flowcount_log_start)?(flow_count-os.flowcount_log_start):0;
		}

		size_t size = strlen(tmp_buffer);
		snprintf(tmp_buffer + size, TMP_BUFFER_SIZE - size , "%lu", lvalue);
		strcat_P(tmp_buffer, PSTR(",\""));
		strcat_P(tmp_buffer, log_type_names+type*3);
		strcat_P(tmp_buffer, PSTR("\","));

		switch(type) {
			case LOGDATA_FLOWSENSE:
				lvalue = (curr_time>os.sensor1_active_lasttime)?(curr_time-os.sensor1_active_lasttime):0;
				break;
			case LOGDATA_SENSOR1:
				lvalue = (curr_time>os.sensor1_active_lasttime)?(curr_time-os.sensor1_active_lasttime):0;
				break;
			case LOGDATA_SENSOR2:
				lvalue = (curr_time>os.sensor2_active_lasttime)?(curr_time-os.sensor2_active_lasttime):0;
				break;
			case LOGDATA_RAINDELAY:
				lvalue = (curr_time>os.raindelay_on_lasttime)?(curr_time-os.raindelay_on_lasttime):0;
				break;
			case LOGDATA_WATERLEVEL:
				lvalue = os.iopts[IOPT_WATER_PERCENTAGE];
				break;
		}
		size = strlen(tmp_buffer);
		snprintf(tmp_buffer + size, TMP_BUFFER_SIZE - size , "%lu", lvalue);
	}
	strcat_P(tmp_buffer, PSTR(","));
	size_t size = strlen(tmp_buffer);
	snprintf(tmp_buffer + size, TMP_BUFFER_SIZE - size , "%lu", curr_time);
	if((os.iopts[IOPT_SENSOR1_TYPE]==SENSOR_TYPE_FLOW) && (type==LOGDATA_STATION)) {
		// RAH implementation of flow sensor
		strcat_P(tmp_buffer, PSTR(","));
		#if defined(ARDUINO)
		dtostrf(flow_last_gpm,5,2,tmp_buffer+strlen(tmp_buffer));
		#else
		snprintf(tmp_buffer+strlen(tmp_buffer), TMP_BUFFER_SIZE, "%5.2f", flow_last_gpm);
		#endif
	}
	strcat_P(tmp_buffer, PSTR("]\r\n"));

#if defined(ARDUINO)
	#if defined(ESP8266)
	file.write((const uint8_t*)tmp_buffer, strlen(tmp_buffer));
	#else
	file.write(tmp_buffer);
	#endif
	file.close();
#else
	fwrite(tmp_buffer, 1, strlen(tmp_buffer), file);
	fclose(file);
#endif
}

#if defined(ESP8266)
bool delete_log_oldest() {
	Dir dir = LittleFS.openDir(LOG_PREFIX);
	time_os_t oldest_t = ULONG_MAX;
	String oldest_fn;
	while (dir.next()) {
		time_os_t t = dir.fileCreationTime();
		if(t<oldest_t) {
			oldest_t = t;
			oldest_fn = dir.fileName();
		}
	}
	if(oldest_fn.length()>0) {
		DEBUG_PRINT(F("deleting "))
		DEBUG_PRINTLN(LOG_PREFIX+oldest_fn);
		LittleFS.remove(LOG_PREFIX+oldest_fn);
		return true;
	} else {
		return false;
	}
}
#endif

/** Delete log file
 * If name is 'all', delete all logs
 */
void delete_log(char *name) {
	if (!os.iopts[IOPT_ENABLE_LOGGING]) return;
#if defined(ARDUINO)

	#if defined(ESP8266)
	if (strncmp(name, "all", 3) == 0) {
		// delete all log files
		Dir dir = LittleFS.openDir(LOG_PREFIX);
		while (dir.next()) {
			LittleFS.remove(LOG_PREFIX+dir.fileName());
		}
	} else {
		// delete a single log file
		make_logfile_name(name);
		if(!LittleFS.exists(tmp_buffer)) return;
		LittleFS.remove(tmp_buffer);
	}
	#else
	if (strncmp(name, "all", 3) == 0) {
		// delete the log folder
		SdFile file;

		if (sd.chdir(LOG_PREFIX)) {
			// delete the whole log folder
			sd.vwd()->rmRfStar();
		}
	} else {
		// delete a single log file
		make_logfile_name(name);
		if (!sd.exists(tmp_buffer))  return;
		sd.remove(tmp_buffer);
	}
	#endif

#else // delete_log implementation for RPI/LINUX
	if (strncmp(name, "all", 3) == 0) {
		// delete the log folder
		rmdir(get_filename_fullpath(LOG_PREFIX));
		return;
	} else {
		make_logfile_name(name);
		remove(get_filename_fullpath(tmp_buffer));
	}
#endif
}

/** Perform network check
 * This function pings the router
 * to check if it's still online.
 * If not, it re-initializes Ethernet controller.
 */
static void check_network() {
#if defined(OS_AVR)
	// do not perform network checking if the controller has just started, or if a program is running
	if (os.status.program_busy) {return;}

	// check network condition periodically
	if (os.status.req_network) {
		os.status.req_network = 0;
		// change LCD icon to indicate it's checking network
		if (!ui_state) {
			os.lcd.setCursor(LCD_CURSOR_NETWORK, 1);
			os.lcd.write('>');
		}


		boolean failed = false;
		// todo: ping gateway ip
		/*ether.clientIcmpRequest(ether.gwip);
		ulong start = millis();
		// wait at most PING_TIMEOUT milliseconds for ping result
		do {
			ether.packetLoop(ether.packetReceive());
			if (ether.packetLoopIcmpCheckReply(ether.gwip)) {
				failed = false;
				break;
			}
		} while(millis() - start < PING_TIMEOUT);*/
		if (failed)  {
			if(os.status.network_fails<3)  os.status.network_fails++;
			// clamp it to 6
			//if (os.status.network_fails > 6) os.status.network_fails = 6;
		}
		else os.status.network_fails=0;
		// if failed more than 3 times, restart
		if (os.status.network_fails==3) {
			// mark for safe restart
			os.nvdata.reboot_cause = REBOOT_CAUSE_NETWORK_FAIL;
			os.status.safe_reboot = 1;
		} else if (os.status.network_fails>2) {
			// if failed more than twice, try to reconnect
			if (os.start_network())
				os.status.network_fails=0;
		}
	}
#else
	// nothing to do for other platforms
#endif
}

/** Perform NTP sync */
static void perform_ntp_sync() {
#if defined(ARDUINO)
	// do not perform ntp if this option is disabled, or if a program is currently running
	if (!os.iopts[IOPT_USE_NTP] || os.status.program_busy) return;
	// do not perform ntp if network is not connected
	if (!os.network_connected()) return;

	if (os.status.req_ntpsync) {
		os.status.req_ntpsync = 0;
		if (!ui_state) {
			os.lcd_print_line_clear_pgm(PSTR("NTP Syncing..."),1);
		}
		DEBUG_PRINTLN(F("NTP Syncing..."));
		static ulong last_ntp_result = 0;
		ulong t = getNtpTime();
		if(last_ntp_result>3 && t>last_ntp_result-3 && t<last_ntp_result+3) {
			DEBUG_PRINTLN(F("error: result too close to last"));
			t = 0;	// invalidate the result
		} else {
			last_ntp_result = t;
		}
		if (t>0) {
			setTime(t);
			RTC.set(t);
			DEBUG_PRINTLN(RTC.get());
		}
	}
#else
	// nothing to do here
	// Linux will do this for you
#endif
}

#if !defined(ARDUINO) // main function for RPI/LINUX
int main(int argc, char *argv[]) {
	// Disable buffering to work with systemctl journal
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("Starting OpenSprinkler\n");

	int opt;
	while(-1 != (opt = getopt(argc, argv, "d:"))) {
		switch(opt) {
		case 'd':
			set_data_dir(optarg);
			break;
		default:
			// ignore options we don't understand
			break;
		}
	}

	do_setup();

	while(true) {
		do_loop();
	}
	return 0;
}
#endif
