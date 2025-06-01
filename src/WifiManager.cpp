#include <WiFi.h>
#include <Logger.h>
#include <WifiManager.h>

#define WIFI_CREDENTIALS_FILE "/wifi_credentials.txt"
#define AP_SSID "ESP32_AP"
#define WIFI_TIMEOUT_MS 20000 // 20 second WiFi connection timeout
#define WIFI_RECOVER_TIME_MS 30000 // Wait 30 seconds after a failed connection attempt

using namespace RidenDongle;

WifiManagerClass RidenDongle::WifiManager;

void WifiManagerClass::begin()
{
    if (readCredentials())
    {
        Serial.println("WiFi credentials read successfully.");
        Serial.printf("SSID: %s, Password: %s\n", ssid.c_str(), _password.c_str());
        connect();
    }
    else
    {
        Serial.println("WiFi credentials not found or invalid. Starting AP mode.");
        startAPMode();
    }
}

bool WifiManagerClass::clearCredentials()
{
    if (LittleFS.remove(WIFI_CREDENTIALS_FILE))
    {
        WiFi.eraseAP();
        LittleFS.end();
        ESP.restart();
        return true;
    }
    return false;
}

bool WifiManagerClass::readCredentials()
{
  File file = LittleFS.open(WIFI_CREDENTIALS_FILE, FILE_READ);
  if (!file)
  {
    Serial.println("Failed to open wifi credentials file");
    return false;
  }

  ssid = file.readStringUntil('\n');
  _password = file.readStringUntil('\n');

  // Remove newline characters
  ssid.trim();
  _password.trim();

  file.close();

  return !ssid.isEmpty() && !_password.isEmpty();
}

void WifiManagerClass::startAPMode()
{
  WiFi.softAP(AP_SSID);

  Serial.println("Access Point started:");
  Serial.print("SSID: ");
  Serial.println(AP_SSID);
  Serial.print("IP Address: ");
  Serial.println(WiFi.softAPIP());
}

void keepWifiAlive(void * parameter){
    WifiManager.keepAlive();
}

void WifiManagerClass::keepAlive() {
    for(;;){
        if(WiFi.status() == WL_CONNECTED){
            vTaskDelay(10000 / portTICK_PERIOD_MS);
            continue;
        }

        LOG_LN("[WIFI] Connecting");
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, _password);

        unsigned long startAttemptTime = millis();

        // Keep looping while we're not connected and haven't reached the timeout
        while (WiFi.status() != WL_CONNECTED && 
                millis() - startAttemptTime < WIFI_TIMEOUT_MS){}

        // When we couldn't make a WiFi connection (or the timeout expired)
		  // sleep for a while and then retry.
        if(WiFi.status() != WL_CONNECTED){
            LOG_LN("[WIFI] FAILED");
            vTaskDelay(WIFI_RECOVER_TIME_MS / portTICK_PERIOD_MS);
			  continue;
        }

        LOG_LN("[WIFI] Connected: " + WiFi.localIP().toString());
    }
}

void WifiManagerClass::connect()
{
    WiFi.mode(WIFI_STA);
    delay(200);
  WiFi.begin(ssid.c_str(), _password.c_str());

  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10)
  {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("Connected to WiFi successfully.");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    xTaskCreatePinnedToCore(
	    keepWifiAlive,
	    "keepWiFiAlive",  // Task name
	    5000,             // Stack size (bytes)
	    NULL,             // Parameter
	    1,                // Task priority
	    NULL,             // Task handle
	    ARDUINO_RUNNING_CORE
    );     
  }
  else
  {
    Serial.println("Failed to connect to WiFi. Starting AP mode.");
    startAPMode();
  }
}

void WifiManagerClass::setCredentials(const String &ssid, const String &password)
{
    this->ssid = ssid;
    _password = password;
}

bool WifiManagerClass::saveCredentials()
{
  File file = LittleFS.open(WIFI_CREDENTIALS_FILE, FILE_WRITE);
  if (!file)
  {
    Serial.println("Failed to open wifi credentials file for writing");
    return false;
  }

  file.println(ssid);
  file.println(_password);

  file.close();
  return true;
}
