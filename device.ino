// Designed for MKR1000
// https://store.arduino.cc/arduino-mkr1000-wifi

// Laitteen toimintaperiaate

// Ideana on lukea pelin voittajan sekä häviäjän kulkukorteista tunnisteet ja
// lähettää tiedot palvelimelle pelien tilastointia varten. Voittaja skannaa
// korttinsa ensin, sitten häviäjä. Tasapeli voidaan toteuttaa myöhemmin
// vaikkapa pohjaan jäävän kytkimen avulla.

// 1.
// Ensin laite haluaa lukea tunnisteen voittajan kulkukortista. Luettu tunniste
// vaihdetaan käyttäjänimeen palvelimen kanssa, joka myös näytetään LCD-näytöllä.
// Jos tunnistetta ei löydy tietokannasta näytetään kyseinen tunniste.

// 2.
// Seuraavaksi laite haluaa lukea häviäjän kortin, X sekunnin sisällä. Jos
// aikaa kuluu enemmän kuin X palataan kohtaan 1. Tarpeeksi nopean kortinluvun
// jälkeen tunniste vaihdetaan taas käyttäjänimeen joka myös näytetään hetken
// aikaa LCD-näytöllä.

// 3.
// Laite lähettää molempien pelaajien nimet palvelimelle tilastointia varten.
// Palataan kohtaan 1.

#include <WiFi101.h>
#include <MFRC522.h>
#include <LiquidCrystal.h>
#include "secrets.h"

// From "secrets.h"
const char *serverIP = SERVER_IP;
uint16_t serverPort = SERVER_PORT;
const char *wifiName = WIFI_SSID;
const char *wifiPassword = WIFI_PASSWORD;

LiquidCrystal lcd(0, 1, 2, 3, 4, 5);

// Main state. How many players is currently readed
int playersReaded = 0;

// ==========================================================================
// RFID-TAGS
// Small delay between reading rfid-tags
// ==========================================================================
unsigned long lastTagTime = 0;
const unsigned long TAG_DELAY = 3000;

// =======================================================================
// TIMEOUT
// If only 1 (one) player is readed for X sec, system timeouts and resets to
// initial state
// =======================================================================
bool canTimeout = false;
unsigned long timeoutTimer = 0;
const int TIMEOUT_DELAY = 7000;

// ==========================================================================
// REQUESTS
// First tags are changed to usernames
// When we have readed both players we are available to save game
// ==========================================================================
bool canAuthenticate = false;
bool canSave = false;

// How RC522 is connected to MKR1000
#define SDA_PIN 11
#define RST_PIN 15

// When reading response, skip headers before interesting data comes
const int HEADER_ROWS_TO_SKIP = 5;
// Read only x bytes from interesting part
// Username can't be longer than this
const int RESPONSE_BUFFER = 64 + 1;
// State when save request has sent
const int GAME_SAVED = -1;

// defined in server
String SUCCESS_MESSAGE = "ok";

// These will be set when server responses
String winnerName = "";
String loserName = "";
String winnerShort = "";
String loserShort = "";
// Readed tag
String tag = "";

MFRC522 mfrc522(SDA_PIN, RST_PIN);
WiFiClient client;

void loop()
{
	// All internet related stuff happens here
	handleTrafficBetweenClientAndServer();

	// Second player must been read after X seconds or we go to initial state
	checkTimeout();

	// Do nothing if there is no RFID-tag to read
	if (!isTagAvailable()) return;

	// Tag available!
	// Make sure there is some delay between reads
	if ((millis() - lastTagTime) > TAG_DELAY) {
		// In next iteration we are allowed to make a request
		canAuthenticate = true;
		// Just to be sure
		canSave = false;
		tag = readTag();
		Serial.println("tag: " + tag + " readed");
		// Update latest tag read timestamp
		lastTagTime = millis();
	}

}

// =======================================================================
// INTERNET FUNCTIONS
// =======================================================================

void handleTrafficBetweenClientAndServer() {
	if (client.connect(serverIP, serverPort)) {
		// Check if we can recognize player or save a game
		// save
		if (canSave)
		{
			Serial.println("BOTH PLAYERS READ, SAVE AVAILABLE!");
			Serial.println("Winner: " + winnerName);
			Serial.println("Loser: " + loserName);
			makeRequest(true, "/api/iot/", winnerName + "," + loserName);
			canSave = false;
			playersReaded = GAME_SAVED;
		}
		// recognize player
		else
		{
			int error = makeRequest(canAuthenticate, "/api/tag/", tag);
			if (error)
				return ;
		}

		// Read response
		while (client.connected())
		{
			if (client.available())
			{
				readResponse();
			}
		}
		client.stop();
		canAuthenticate = false;
	}
	// Not connected
	else
	{
		handleServerOffline();
	}
}

void handleServerOffline() {
	if (!(canAuthenticate || canSave)) return ;

	showMessage("Server offline", 2000);
	resetState();
	// Debug
	Serial.println("Server offline");
	Serial.print("playersReaded: ");
	Serial.println((int)playersReaded);
	showMainScreen();
}

int makeRequest(bool isAllowed, String url, String payload) {
	if (!isAllowed)
		return 1;
	Serial.println("Sending request with: <" + tag + ">");

	client.print("GET ");
	client.print(url);
	client.print(payload);
	client.println(" HTTP/1.1");

	client.print("Host: ");
	client.println(serverIP);
	client.println("User-Agent: ArduinoWiFi/1.1");
	client.print("Arduino-Token: ");
	client.println(SECRET_TOKEN);
	client.println("Connection: close");
	client.println();
	return 0;
}

void readResponse() {
	int newLines = 0;
	int i = 0;
	char buffer[RESPONSE_BUFFER + 1];
	while (client.connected()) {
		// Client aint connected to server anymore so we should have a response
		if (!client.available())
		{

			// Response is now available!

			Serial.println("");
			Serial.print("Response (buffer): ");
			Serial.println(buffer);


			String temp(buffer);
			// One player readed, not in state yet
			if (playersReaded == 0) {
				winnerName = temp;

				if (winnerName.length() > 16) {
					winnerShort = temp.substring(0, 12);
					winnerShort += "...";
				} else {
					winnerShort = temp.substring(0, 15);
				}

				showOnePlayer(winnerShort);
				startTimeoutClock();
			}
			// Two players readed, not in state yet
			else if (playersReaded == 1) {
				disableTimeoutClock();
				loserName = temp;
				if (loserName.length() > 16) {
					loserShort = temp.substring(0, 12);
					loserShort += "...";
				} else {
					loserShort = temp.substring(0, 15);
				}
				//loserShort = temp.substring(0, 15);
				showBothPlayers(winnerShort, loserShort, 2000);
			}


			// Update state
			// After game is saved, reset state
			if (playersReaded == GAME_SAVED) {
				String message = "Game uploaded!";
				if (!temp.equals(SUCCESS_MESSAGE)) {
					message = "Save failed!";
				}
				showMessage(message, 2000);
				resetState();
				showMainScreen();
			}

			// Otherwise we have read one player more so increase state
			else {
				playersReaded++;
			}

			// Debug
			Serial.print("playersReaded: ");
			Serial.println((int)playersReaded);

			// When both players are read we are able to save a game
			if (playersReaded == 2) {
				canSave = true;
				canAuthenticate = false;
			}
			return ;
		}

		// Parse response
		char c = client.read();
		// Calculate newlines so we can ignore headers
		if (c == '\n')
			newLines++;
		// Ignore first rows
		if (newLines > HEADER_ROWS_TO_SKIP && c != '\n') {
			// Copy only X char
			if (i < RESPONSE_BUFFER) {
				buffer[i] = c;
			}
			i++;
		}
		if (i < RESPONSE_BUFFER) {
			buffer[i] = '\0';
		}
		buffer[RESPONSE_BUFFER] = '\0';
	}
}

// =======================================================================
// TIMEOUT FUNCTIONS
// =======================================================================

void checkTimeout() {
	if (!(canTimeout && (millis() - timeoutTimer) >= TIMEOUT_DELAY)) {
		return;
	}
	showMessage("Timed out!", 2000);
	Serial.println("TIMED OUT!");
	resetState();
	Serial.print("Players readed: ");
	Serial.println((int)playersReaded);
	showMainScreen();
}

void startTimeoutClock() {
	canTimeout = true;
	timeoutTimer = millis();
}

void disableTimeoutClock() {
	canTimeout = false;
}


void resetState() {
	playersReaded = 0;
	canAuthenticate = false;
	canTimeout = false;
	canSave = 0;
	winnerName = "";
	loserName = "";
}

// =======================================================================
// RFID FUNCTIONS
// =======================================================================

int isTagAvailable()
{
	if (!mfrc522.PICC_IsNewCardPresent())
		return 0;
	if (!mfrc522.PICC_ReadCardSerial())
		return 0;
	return 1;
}

String readTag()
{
	String tag = "";
	for (byte i = 0; i < mfrc522.uid.size; i++)
	{
		tag.concat(String(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " "));
		tag.concat(String(mfrc522.uid.uidByte[i], HEX));
	}
	tag.toUpperCase();
	tag.replace(" ", "%20");
	return tag;
}

// =======================================================================
// WIFI FUNCTIONS
// =======================================================================

// Blocks until WiFi-connections is established
void connectToWiFi(const char *ssid, const char *password)
{
	if (WiFi.status() == WL_NO_SHIELD)
	{
		Serial.println("WiFi not enabled");
		while (true)
		{
		}
	}

	int status = WL_IDLE_STATUS;
	while (status != WL_CONNECTED)
	{
		Serial.println("Trying connect to WiFi");
		status = WiFi.begin(ssid, password);
		Serial.print("Status: ");
		Serial.println(status);
		delay(5000);
	}
}

void printWifiStatus()
{
	// Print the SSID of the network you're attached to:
	Serial.print("SSID: ");
	Serial.println(WiFi.SSID());

	// Print your WiFi shield's IP address:
	IPAddress ip = WiFi.localIP();
	Serial.print("IP Address: ");
	Serial.println(ip);

	// Print the received signal strength:
	long rssi = WiFi.RSSI();
	Serial.print("signal strength (RSSI):");
	Serial.print(rssi);
	Serial.println(" dBm");
	// Print where to go in a browser:
	Serial.print("To see this page in action, open a browser to http://");
	Serial.println(ip);
}

// =======================================================================
// LCD FUNCTIONS
// =======================================================================

void showMessage(String msg) {
	lcd.clear();
	lcd.setCursor(0,0);
	lcd.print(msg);
}

void showMessage(String msg, int delayMillis) {
	lcd.clear();
	lcd.setCursor(0,0);
	lcd.print(msg);
	delay(delayMillis);
}

void showMainScreen() {
	lcd.clear();
	lcd.setCursor(0,0);
	lcd.print("Read 2 players,");
	lcd.setCursor(0,1);
	lcd.print("winner first");
}

void showOnePlayer(String player) {
	lcd.clear();
	lcd.setCursor(0, 0);
	lcd.print(player);
	lcd.setCursor(0, 1);
	lcd.print("Read another");
}

void showBothPlayers(String player1, String player2, int delayMillis) {
	lcd.clear();
	lcd.setCursor(0, 0);
	lcd.print(player1);
	lcd.setCursor(0, 1);
	lcd.print(player2);
	delay(delayMillis);
}

// =======================================================================
// SETUP FUNCTION
// =======================================================================

void setup()
{
	// This is needed to communicate with RC522
	SPI.begin();
	mfrc522.PCD_Init();
	Serial.begin(115200);
	Serial.println("Card reader ready...");
	lcd.begin(16, 2);
	showMessage("Connecting...", 1000);
	pinMode(LED_BUILTIN, OUTPUT);
	digitalWrite(LED_BUILTIN, HIGH);
	connectToWiFi(wifiName, wifiPassword);
	printWifiStatus();
	showMessage("Connected", 1000);
	client.connect(serverIP, serverPort);
	// Debug
	Serial.print("playersReaded: ");
	Serial.println((int)playersReaded);
	showMainScreen();
}
