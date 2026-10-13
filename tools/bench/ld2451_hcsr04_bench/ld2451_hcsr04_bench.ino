// Bench test for the LD2451 radar and HC-SR04 ultrasonic on an ESP32 DevKit.
// Test tool only -- not product code. The product runs on the Pi 5.
//
// Wiring (ESP32 on USB power). Board silkscreen label first, GPIO in parens;
// Dn == GPIOn, and GPIO16/17 are usually printed RX2/TX2.
//   LD2451  VCC -> VIN (5V; confirm against datasheet)   GND -> GND
//   LD2451  TX  -> RX2 (GPIO16)                          RX  -> TX2 (GPIO17)
//   HC-SR04 VCC -> VIN (5V)                              GND -> GND
//   HC-SR04 TRIG -> D18
//   HC-SR04 ECHO -> 1k -> D19, and 2k from D19 to GND (5V -> ~3.3V)
//   LED1 radar link   D25 -> 220-330R -> LED -> GND
//   LED2 radar target D26 -> 220-330R -> LED -> GND
//   LED3 sonar near   D27 -> 220-330R -> LED -> GND
//
// Serial monitor: 115200. Output lines:
//   RADAR <ms> <len> <hex bytes of whole frame>
//   SONAR <ms> <cm> | SONAR <ms> timeout

#define LOG_BAUD        115200
#define RADAR_BAUD      115200   // presumed LD2451 default
#define RADAR_RX_PIN    16
#define RADAR_TX_PIN    17
#define SONAR_TRIG_PIN  18
#define SONAR_ECHO_PIN  19
#define LED_LINK_PIN    25
#define LED_TARGET_PIN  26
#define LED_NEAR_PIN    27

#define FRAME_MAX       128      // header(4) + len(2) + payload + tail(4)
// UNVERIFIED: index of the target-count byte within the whole frame.
// Confirm against the datasheet and the raw RADAR dumps before trusting LED2.
#define TARGET_COUNT_OFFSET 6

#define RADAR_SILENT_MS 2000
#define LINK_BLINK_MS   50
#define SONAR_PERIOD_MS 60
#define SONAR_TIMEOUT_US 30000UL
#define NEAR_CM         100

static const uint8_t HDR[4]  = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t TAIL[4] = {0xF8, 0xF7, 0xF6, 0xF5};

static uint8_t frame[FRAME_MAX];
static size_t frame_n;
static size_t frame_len;          // payload length from the frame's length field
static uint32_t bad_frames;

static unsigned long last_radar_byte_ms;
static unsigned long last_radar_frame_ms;
static unsigned long last_warn_ms;
static unsigned long link_on_ms;
static unsigned long last_sonar_ms;

static void handle_frame(void)
{
	unsigned long now = millis();
	size_t total = 10 + frame_len;

	Serial.printf("RADAR %lu %u", now, (unsigned)frame_len);
	for (size_t i = 0; i < total; i++)
		Serial.printf(" %02X", frame[i]);
	Serial.print('\n');

	last_radar_frame_ms = now;
	link_on_ms = now;
	digitalWrite(LED_LINK_PIN, HIGH);

	// Offset must land inside the payload, not the tail; an empty
	// (len 0) frame means no targets.
	if (6 + frame_len > TARGET_COUNT_OFFSET)
		digitalWrite(LED_TARGET_PIN, frame[TARGET_COUNT_OFFSET] > 0 ? HIGH : LOW);
	else
		digitalWrite(LED_TARGET_PIN, LOW);
}

// Byte-at-a-time frame sync: match header, read length, wait for tail.
static void radar_feed(uint8_t b)
{
	if (frame_n < 4) {
		if (b == HDR[frame_n]) {
			frame[frame_n++] = b;
		} else if (b == HDR[0]) {
			frame[0] = b;
			frame_n = 1;
		} else {
			frame_n = 0;
		}
		return;
	}

	frame[frame_n++] = b;

	if (frame_n == 6) {
		frame_len = (size_t)frame[4] | ((size_t)frame[5] << 8);
		if (10 + frame_len > FRAME_MAX) {
			bad_frames++;
			Serial.printf("RADAR bad length %u (bad=%lu)\n",
				      (unsigned)frame_len, (unsigned long)bad_frames);
			frame_n = 0;
		}
		return;
	}

	if (frame_n > 6 && frame_n == 10 + frame_len) {
		if (memcmp(&frame[6 + frame_len], TAIL, sizeof(TAIL)) == 0) {
			handle_frame();
		} else {
			bad_frames++;
			Serial.printf("RADAR bad tail (bad=%lu)\n", (unsigned long)bad_frames);
		}
		frame_n = 0;
	}
}

static void radar_poll(void)
{
	while (Serial2.available() > 0) {
		int c = Serial2.read();
		if (c < 0)
			break;
		last_radar_byte_ms = millis();
		radar_feed((uint8_t)c);
	}
}

static void sonar_measure(void)
{
	digitalWrite(SONAR_TRIG_PIN, LOW);
	delayMicroseconds(2);
	digitalWrite(SONAR_TRIG_PIN, HIGH);
	delayMicroseconds(10);
	digitalWrite(SONAR_TRIG_PIN, LOW);

	unsigned long us = pulseIn(SONAR_ECHO_PIN, HIGH, SONAR_TIMEOUT_US);
	unsigned long now = millis();

	if (us == 0) {
		Serial.printf("SONAR %lu timeout\n", now);
		digitalWrite(LED_NEAR_PIN, LOW);
		return;
	}

	unsigned long cm = us / 58;
	Serial.printf("SONAR %lu %lu\n", now, cm);
	digitalWrite(LED_NEAR_PIN, cm < NEAR_CM ? HIGH : LOW);
}

void setup()
{
	Serial.begin(LOG_BAUD);

	pinMode(LED_LINK_PIN, OUTPUT);
	pinMode(LED_TARGET_PIN, OUTPUT);
	pinMode(LED_NEAR_PIN, OUTPUT);
	pinMode(SONAR_TRIG_PIN, OUTPUT);
	pinMode(SONAR_ECHO_PIN, INPUT);
	digitalWrite(SONAR_TRIG_PIN, LOW);

	// Room for ~90ms of radar bytes while pulseIn blocks.
	Serial2.setRxBufferSize(1024);
	Serial2.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);

	Serial.println("bench: LD2451 + HC-SR04 start");
}

void loop()
{
	unsigned long now = millis();

	radar_poll();

	if (now - link_on_ms >= LINK_BLINK_MS)
		digitalWrite(LED_LINK_PIN, LOW);

	if (now - last_radar_frame_ms >= RADAR_SILENT_MS)
		digitalWrite(LED_TARGET_PIN, LOW);

	if (now - last_radar_byte_ms >= RADAR_SILENT_MS &&
	    now - last_warn_ms >= RADAR_SILENT_MS) {
		Serial.println("RADAR no data: check 5V power, TX/RX swap, baud");
		last_warn_ms = now;
	}

	if (now - last_sonar_ms >= SONAR_PERIOD_MS) {
		last_sonar_ms = now;
		sonar_measure();
		radar_poll();
	}
}
