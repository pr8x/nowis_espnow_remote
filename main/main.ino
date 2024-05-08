#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <ezButton.h>
#include <ESP32Encoder.h>

#include <array>
#include <vector>

#include <TFT_eSPI.h>  // Graphics and font library for ST7735 driver chip
#include <SPI.h>

TFT_eSPI tft = TFT_eSPI();  // Invoke library, pins defined in User_Setup.h

struct peer {
  String name;
  std::array<uint8_t, 6> mac;
};

std::array<uint8_t, 6> parse_mac(const char *macStr) {
  std::array<uint8_t, 6> mac = {};
  if (sscanf(macStr, "%x:%x:%x:%x:%x:%x",
             &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5])
      != 6) {
    //TODO
  }

  return mac;
}

void format_mac(char *buffer, const uint8_t *mac) {
  sprintf(buffer, "%x:%x:%x:%x:%x:%x",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

std::array<peer, 4> Peers = { {

  { "All", { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } },
  { "Q1", parse_mac("B0:A7:32:27:E4:30") },
  { "Q2", parse_mac("B0:A7:32:27:E4:31") },
  { "Q3", parse_mac("B0:A7:32:27:E4:32") }

} };

std::vector<String> LogHistory;

uint32_t ActivePeerMask = 0x1 << 1;

ezButton OffButton(16);
ezButton Mode1Button(5);
ezButton Mode2Button(21);
ezButton Mode3Button(22);
ezButton Mode4Button(17);
ezButton TargetSelectButton(27);
ESP32Encoder TargetSelectRotary;
int SelectedTarget = 0;
int LastMode = -1;

constexpr int MaxBroadcastTries = 5;

// This is the same message structure as WizMote senders. It's reused here to
// ensure a common parsable payload, but everything other than seq and button
// aren't used by WLED. The ESP-NOW packet has 512 bytes reserved for the message
// anyway, though, so the cruft shouldn't affect anything
typedef struct remote_message_struct {
  uint8_t program;      // 0x91 for ON button, 0x81 for all others
  uint8_t seq[4];       // Incremetal sequence number 32 bit unsigned integer LSB first
  uint8_t byte5 = 32;   // Unknown
  uint8_t button;       // Identifies which button is being pressed
  uint8_t byte8 = 1;    // Unknown, but always 0x01
  uint8_t byte9 = 100;  // Unnkown, but always 0x64

  uint8_t byte10;  // Unknown, maybe checksum
  uint8_t byte11;  // Unknown, maybe checksum
  uint8_t byte12;  // Unknown, maybe checksum
  uint8_t byte13;  // Unknown, maybe checksum
} remote_message_struct;

#define WIZMOTE_BUTTON_ON 1
#define WIZMOTE_BUTTON_OFF 2
#define WIZMOTE_BUTTON_NIGHT 3
#define WIZMOTE_BUTTON_ONE 16
#define WIZMOTE_BUTTON_TWO 17
#define WIZMOTE_BUTTON_THREE 18
#define WIZMOTE_BUTTON_FOUR 19
#define WIZMOTE_BUTTON_BRIGHT_UP 9
#define WIZMOTE_BUTTON_BRIGHT_DOWN 8

remote_message_struct outgoing;
uint32_t seq = 1;
bool uiUpdatePending = false;

template<class... Args>
void log(const char *m, const Args &...args) {
  char buffer[64];
  sprintf(buffer, m, args...);

  Serial.printf("%s\n", buffer);
  LogHistory.emplace_back(buffer);
  queue_ui_update();
}

void send_press(int buttonCode) {
  Serial.printf("Sending button code %d\n", buttonCode);

  // increment message sequence counter
  seq = seq + 1;

  // format sequence counter long data in the expected format
  outgoing.seq[0] = seq;
  outgoing.seq[1] = seq >> 8;
  outgoing.seq[2] = seq >> 16;
  outgoing.seq[3] = seq >> 24;

  outgoing.button = buttonCode;

  broadcast_packets();
}

void broadcast_packets() {
  Serial.printf("Peer mask: %d\n", ActivePeerMask);

  for (int p = 0; p < Peers.size(); ++p) {
    if (ActivePeerMask & (0x1 << p)) {

      const auto &peer = Peers[p];

      char macBuf[32];
      format_mac(macBuf, peer.mac.data());
      Serial.printf("Broadcasting to %s [%s]...\n", peer.name.c_str(), macBuf);

      for (int i = 0; i <= 14; ++i) {
        esp_wifi_set_channel(i, WIFI_SECOND_CHAN_NONE);
        esp_now_send(peer.mac.data(), (uint8_t *)&outgoing, sizeof(outgoing));
      }
    }
  }
}

void setup() {
  // Init Serial Monitor
  Serial.begin(115200);

  //Serial.setDebugOutput(true);

  // Set station mode, though we won't be connecting to another AP
  WiFi.mode(WIFI_STA);

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    log("Error initializing ESP-NOW");
    return;
  }

  // Set ESP NOW to controller (sender) mode
  //esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);

  // Once ESPNow is successfully Init, we will register for Send CB to
  // get the status of Trasnmitted packet
  //esp_now_register_send_cb(on_data_sent);

  esp_now_peer_info_t peerInfo{};
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  for (auto &peer : Peers) {
    memcpy(peerInfo.peer_addr, peer.mac.data(), 6);

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      log("Failed to add peer: %s", peer.name.c_str());
      return;
    } else {
      log("Registered peer: %s", peer.name.c_str());
    }
  }

  tft.init();
  tft.setRotation(3);
  log("Init TFT");

  TargetSelectButton.setDebounceTime(100);
  TargetSelectRotary.attachHalfQuad(25, 14);
  TargetSelectRotary.setCount(0);

  //https://github.com/madhephaestus/ESP32Encoder?tab=readme-ov-file#a-note-on-ky-040-and-similar
  TargetSelectRotary.setFilter(1023);

  queue_ui_update();
}

void queue_ui_update() {
  uiUpdatePending = true;
}

void update_ui() {
  Serial.println("update_ui");

  tft.begin();
  tft.fillScreen(TFT_BLACK);

  int x = 5;
  int y = 5;

  tft.drawString("Targets:", x, y);
  y += 10;

  for (int p = 0; p < Peers.size(); ++p) {
    auto name = Peers[p].name.c_str();
    auto width = tft.textWidth(name);

    bool isActive = ActivePeerMask & (0x1 << p);
    bool isSelected = SelectedTarget == p;

    if (isActive) {
      tft.fillRect(x - 1, y - 1, width + 1, 10, TFT_BLUE);
    }

    tft.setTextColor(isSelected ? TFT_RED : TFT_WHITE);

    tft.drawString(Peers[p].name.c_str(), x, y);
    x += width + 4;
  }

  tft.setTextColor(TFT_WHITE);

  y += 15;
  tft.drawLine(0, y, TFT_HEIGHT, y, TFT_WHITE);
  y += 5;
  x = 5;

  if (LastMode == -1) {
    tft.drawString("Last mode: Off", x, y);
  } else {
    char modeStr[16];
    sprintf(modeStr, "Last mode: %d", LastMode);

    constexpr uint16_t colors[] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW };
    tft.fillRect(x, y, tft.textWidth(modeStr), 10, colors[LastMode - 1]);
    tft.drawString(modeStr, x, y);
  }

  y += 15;
  tft.drawLine(0, y, TFT_HEIGHT, y, TFT_WHITE);
  y += 5;

  for (auto it = LogHistory.rbegin(); it != LogHistory.rend(); ++it) {
    tft.drawString(it->c_str(), x, y);
    y += 10;
  }

  uiUpdatePending = false;
}

void loop() {

  auto targetSelectCount = TargetSelectRotary.getCount() / 2;

  OffButton.loop();
  Mode1Button.loop();
  Mode2Button.loop();
  Mode3Button.loop();
  Mode4Button.loop();
  TargetSelectButton.loop();

  if (OffButton.isReleased()) {
    send_press(WIZMOTE_BUTTON_OFF);

    LastMode = -1;
    queue_ui_update();
  } else if (Mode1Button.isReleased()) {
    send_press(WIZMOTE_BUTTON_ONE);
    LastMode = 1;
    queue_ui_update();
  } else if (Mode2Button.isReleased()) {
    send_press(WIZMOTE_BUTTON_TWO);
    LastMode = 2;
    queue_ui_update();
  } else if (Mode3Button.isReleased()) {
    send_press(WIZMOTE_BUTTON_THREE);
    LastMode = 3;
    queue_ui_update();
  } else if (Mode4Button.isReleased()) {
    send_press(WIZMOTE_BUTTON_FOUR);
    LastMode = 4;
    queue_ui_update();
  }

  if (TargetSelectButton.isReleased()) {
    ActivePeerMask ^= 0x1 << SelectedTarget;

    if (SelectedTarget == 0) {
      ActivePeerMask = 1;
    } else {
      ActivePeerMask &= ~1;
    }

    Serial.printf("ActivePeerMask: %d\n", ActivePeerMask);
    queue_ui_update();
  }

  if (targetSelectCount != 0) {
    auto dir = targetSelectCount < 0 ? -1 : 1;
    SelectedTarget = (SelectedTarget + dir) % Peers.size();

    TargetSelectRotary.clearCount();
    queue_ui_update();
  }

  if (uiUpdatePending) {
    update_ui();
  }
}