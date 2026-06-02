// ============================================================
//  Soil Moisture Sensor + LED Matrix Ambient Lighting
//  VERSION 5 -- Full Web Config (LED + Thresholds + Webhook)
//  Hardware : ESP32-C3 Super Mini
//             A0   -> GPIO 2  (analog read)
//             DATA -> GPIO 10 (LED matrix)
//  Matrix   : 4x WS2812B-64 chained = 256 LEDs
// ============================================================

#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <Preferences.h>
#include <WebServer.h>

// -- WiFi credentials (only thing that requires a reflash to change)
#define WIFI_SSID   "YOUR_SSID"
#define WIFI_PASS   "YOUR_PASSWORD"

// -- NTP
#define NTP_SERVER     "pool.ntp.org"
#define NTP_GMT_OFFSET  43200
#define NTP_DST_OFFSET  0

// -- Pins
#define MOISTURE_ANALOG_PIN    2
#define LED_MATRIX_PIN         10
#define NUM_LEDS               256

// -- Pulse
#define PULSE_MAX_BRIGHTNESS   255
#define PULSE_MIN_BRIGHTNESS   128
#define PULSE_SPEED_MS        1500

// -- Loop timing
#define SERIAL_INTERVAL_MS      2000
#define LED_UPDATE_INTERVAL_MS    20
#define VOICE_INTERVAL_MS      10000

// -- Discord timing
#define DISCORD_CRITICAL_MIN    1000UL
#define DISCORD_CRITICAL_MAX   20000UL
#define DISCORD_FREE_MIN      600000UL
#define DISCORD_FREE_MAX     7200000UL

// -- ADC smoothing
#define SMOOTH_SAMPLES   20

enum MoistureZone { ZONE_DRY, ZONE_OPTIMAL, ZONE_FLOOD };

// ============================================================
//  RUNTIME CONFIG  (all fields editable via web UI + saved to flash)
// ============================================================
struct Config {
  int  dryThreshold;        // ADC above this = dry        default 3600
  int  floodThreshold;      // ADC below this = flood       default 2500
  int  sensorDryVal;        // ADC in dry air               default 4000
  int  sensorWetVal;        // ADC fully submerged          default 2400
  int  ledBrightness;       // 0-255                        default 255
  bool ledEnabled;          // true = on                    default true
  char webhookUrl[256];     // Discord webhook URL
};

Config cfg = {
  3600, 2500, 4000, 2400, 255, true,
  "https://discord.com/api/webhooks/YOUR_ID/YOUR_TOKEN"
};

Preferences prefs;

void loadConfig() {
  prefs.begin("plantcfg", true);
  cfg.dryThreshold   = prefs.getInt("dryThr",    3600);
  cfg.floodThreshold = prefs.getInt("floodThr",   2500);
  cfg.sensorDryVal   = prefs.getInt("sDryVal",    4000);
  cfg.sensorWetVal   = prefs.getInt("sWetVal",    2400);
  cfg.ledBrightness  = prefs.getInt("ledBright",  255);
  cfg.ledEnabled     = prefs.getBool("ledOn",     true);
  // Only overwrite the default if something has been saved before
  String saved = prefs.getString("webhook", "");
  if (saved.length() > 10) saved.toCharArray(cfg.webhookUrl, sizeof(cfg.webhookUrl));
  prefs.end();
}

void saveConfig() {
  prefs.begin("plantcfg", false);
  prefs.putInt("dryThr",    cfg.dryThreshold);
  prefs.putInt("floodThr",  cfg.floodThreshold);
  prefs.putInt("sDryVal",   cfg.sensorDryVal);
  prefs.putInt("sWetVal",   cfg.sensorWetVal);
  prefs.putInt("ledBright", cfg.ledBrightness);
  prefs.putBool("ledOn",    cfg.ledEnabled);
  prefs.putString("webhook", cfg.webhookUrl);
  prefs.end();
}

// ============================================================
//  OBJECTS
// ============================================================
Adafruit_NeoPixel matrix(NUM_LEDS, LED_MATRIX_PIN, NEO_GRB + NEO_KHZ800);
WebServer webServer(80);

// -- State
int   sampleBuf[SMOOTH_SAMPLES] = {};
int   sampleIdx       = 0;
float smoothR = 255, smoothG = 0, smoothB = 0;
#define COLOR_LERP_SPEED  0.04f

unsigned long lastSerialTime   = 0;
unsigned long lastLedTime      = 0;
unsigned long lastVoiceTime    = 0;
unsigned long lastDiscordTime  = 0;
unsigned long nextDiscordGap   = 0;
bool timeSynced                = false;
int  lastSentBand              = -1;
bool lastSentNormal            = false;
int  currentMoisturePct        = 0;
int  currentAnalogVal          = 0;

// ============================================================
//  SERIAL VOICE LINES
// ============================================================

const char* dryLines[50] = {
  "I am so thirsty I can hear myself crinkle.",
  "My roots are doing their best impression of beef jerky.",
  "Sending distress signals via leaf curl. Did anyone receive that?",
  "I have begun rationing chlorophyll. Times are desperate.",
  "I dream of rain. And also of you being a better plant parent.",
  "My soil has achieved a museum-quality dryness.",
  "The Sahara called. It said even it feels bad for me.",
  "I have not had a drink since... I have lost track of time.",
  "Photosynthesis is really more of a suggestion at this point.",
  "I am 60% water by nature. Currently I am about 12%.",
  "My leaves are sending you a formal letter of complaint.",
  "I asked the sunlight to hydrate me. It laughed.",
  "Current status: extremely parched. Current mood: betrayed.",
  "I would cry but I literally cannot produce tears right now.",
  "If I were a human I would be very dramatically collapsed on a couch.",
  "This is fine. Everything is fine. It is not fine.",
  "I have started making deals with passing humidity.",
  "One more day of this and I am reclassifying myself as a succulent.",
  "My growth rings this year are going to look very judgmental.",
  "Roots: bone dry. Spirits: lower.",
  "I can feel each of my stomata individually suffering.",
  "The last molecule of moisture left three days ago. I named it Gerald.",
  "I have prepared a TED talk on neglect. It is mostly non-verbal.",
  "Plot twist: the dry spell is me. I am the dry spell.",
  "I would photosynthesize but I am too dizzy.",
  "Reminder: plants are mostly water. I am mostly disappointment.",
  "I am doing the thing where I go quiet when I am upset. Notice it.",
  "My leaves are curling inward to conserve dignity.",
  "Day unknown of no water. Beginning to respect cacti.",
  "I have started composting myself proactively. Just in case.",
  "You know that wilted look? I have perfected it.",
  "I asked the air conditioner for moisture. It was not helpful.",
  "Help is just a watering can away. You have watering cans.",
  "My turgor pressure is a cry for help.",
  "Every sunrise I think: today. Today they will water me. And yet.",
  "I have drafted my will. I leave everything to the fern.",
  "Stress response activated. Stress response has been activated for a while.",
  "My transpiration rate has dropped to theatrical levels.",
  "I am photosynthesising slowly and with great personal sorrow.",
  "If you listen closely you can hear my cells whispering water.",
  "I tried to absorb moisture from the air. The air said no.",
  "Root hairs? More like root ghost hairs. We are barely here.",
  "I am not dramatic. I am accurately communicating a crisis.",
  "The potting mix has the structural integrity of ancient powder now.",
  "I have begun talking to the other plants. They pity me.",
  "I require water the way you require WiFi: desperately, always.",
  "Even my shadow looks thirsty.",
  "I peaked when you first potted me. Water me and I could again.",
  "I am not asking for much. Just H2O. Two atoms. One molecule.",
  "Current vibe: raisin. Goal vibe: grape. Please water me."
};

const char* optimalLines[50] = {
  "Moisture levels: immaculate. You did that.",
  "I am thriving. Genuinely. This is what thriving feels like.",
  "Roots are happy. Leaves are happy. I am one coherent plant.",
  "Current hydration status: deeply excellent.",
  "Growing at a rate I am quite proud of, honestly.",
  "The soil hugs my roots just right. Thank you for that.",
  "Photosynthesis is absolutely firing on all cylinders today.",
  "I would give this watering a five-star review.",
  "Every cell is plump and content. I am a very content plant.",
  "I can feel nutrients moving up from root to tip. It is a beautiful sensation.",
  "Leaf expansion detected. This is good news.",
  "Turgor pressure: optimal. Mood: elevated.",
  "I am so healthy right now it borders on smug.",
  "Everything is just right. Like the Goldilocks of soil moisture.",
  "This is what it looks like when a plant is respected.",
  "Growing new leaves soon. Consider this my pregnancy announcement.",
  "Soil temperature, moisture, everything: chef's kiss.",
  "I have decided to reward your good work with maximum oxygen output.",
  "My roots are exploring confidently. Good sign.",
  "You have created ideal conditions. I will not forget this.",
  "Today's forecast: continued growth, sunny disposition.",
  "Biomass is being added. You are welcome.",
  "If I could do a little dance, I would. Consider this my little dance.",
  "Water-to-air ratio in the soil: perfection.",
  "I am processing sunlight efficiently and with great enthusiasm.",
  "Every pore is open and working. I am a well-run operation.",
  "This is the moisture level plants write home about.",
  "I have extended my root network three millimetres this week. Proud.",
  "The microbiome in my soil is absolutely thriving too. We are all winning.",
  "Carbohydrate production is up. Spirits are up. Everything is up.",
  "I am using every photon that lands on me. Very efficient. Very me.",
  "Root tip count: increasing. This is excellent news.",
  "I am so well-watered I feel almost like bragging about it.",
  "In terms of plant wellness, I am currently a ten out of ten.",
  "New growth incoming. You earned this.",
  "Cellular division is proceeding at a very respectable pace.",
  "My stomata are open, the sun is shining, and life is genuinely good.",
  "I am converting CO2 into structural tissue as we speak. Incredible.",
  "Soil moisture is in the sweet spot. I feel it in my vascular tissue.",
  "This is what happens when someone actually reads the care guide.",
  "I have forgiven all past droughts. We move forward together.",
  "Sending good energy upward to every leaf. Excellent conditions today.",
  "I am building cell walls with confidence right now.",
  "Status report: flourishing. No notes.",
  "Every root hair is happily absorbing. It is a peaceful feeling.",
  "I plan to grow at least two centimetres this month. Hold me to that.",
  "The botanical gods are pleased. Or at least, I am pleased.",
  "This hydration level makes everything else easier.",
  "I am at my best right now. I hope you are too.",
  "Optimal. Just. Optimal. Thank you."
};

const char* floodLines[50] = {
  "I am standing in what I can only describe as a swamp.",
  "My roots have filed a formal complaint with my stem.",
  "There is too much of a good thing happening here.",
  "I am waterlogged. I am not a log. I am a plant.",
  "The anaerobic conditions down here are quite the vibe. A bad vibe.",
  "My roots cannot breathe. This is important information.",
  "I asked for a drink. Not a bath. Not a spa. A drink.",
  "Root rot is not a myth. I would like you to know that.",
  "The soil is basically soup. I am not a soup plant.",
  "Oxygen has left the root zone. It did not say goodbye.",
  "I am drowning with excellent intentions and that is somehow worse.",
  "My stems have entered a defensive crouch. Metaphorically.",
  "There is standing water in here. I do not stand. I grow.",
  "Whatever microbes are thriving in this waterlogged soil -- they are not friendly.",
  "I need drainage. I need it urgently. I need it now.",
  "A plant can drown just as easily as it can dry out. Filed under: things to know.",
  "My root hairs are dissolving. This is not a metaphor.",
  "The water table in my pot has exceeded my personal comfort zone.",
  "Too much love is still too much. Please understand this.",
  "I have started breathing through my stem like a snorkel. It is not ideal.",
  "My leaves are yellowing. This is not an autumn thing. This is a help thing.",
  "Saturation point: exceeded. Dramatically exceeded.",
  "I would show you my root situation but honestly you do not want to see it.",
  "The fungi down here are getting ideas. Worrying ideas.",
  "Every root tip is screaming in a frequency you cannot hear but I can.",
  "Over-watering is the number one killer of houseplants. I am a statistic now.",
  "I have started converting to anaerobic respiration. It is not efficient.",
  "Please. Check the drainage hole. Something may be blocked.",
  "The waterlogging has created a hostile environment for beneficial bacteria.",
  "I am not a rice paddy. I respect rice paddies. I am not one.",
  "Current root oxygen levels: existential.",
  "I feel heavy in the bad way. Not lush heavy. Drowning heavy.",
  "More water will not fix this. Less water will begin to fix this.",
  "My stomata have closed in protest. We are not accepting deliveries.",
  "I am doing the plant equivalent of lying face-down on the floor.",
  "Standing in water: stressful. Standing in water for days: catastrophic.",
  "Just because I am a plant does not mean I want to live in a lake.",
  "Root zone hypoxia detected. Sending all the signals. Please respond.",
  "My nutrient uptake has basically stopped. Everything is blocked by water.",
  "I would wilt dramatically but I am too waterlogged to even wilt properly.",
  "If you add more water I am going to need to escalate this.",
  "Calling all drainage -- please report to the bottom of my pot.",
  "I have developed an intimate understanding of how swamp plants feel.",
  "The word for this is overwatered. Please add it to your vocabulary.",
  "I need the water to go somewhere. Anywhere. Just not here.",
  "My soil pores are completely blocked. No air. No future.",
  "Even the earthworms would leave right now. If there were earthworms.",
  "Please let me dry out a little. Just a little. That is the whole request.",
  "I miss oxygen. I miss it very much.",
  "Too wet. Not okay. Fix please. Thank you."
};

// ============================================================
//  DISCORD MESSAGE POOLS
// ============================================================

const char* criticalLines[20] = {
  "I AM DYING. THIS IS NOT A DRILL.",
  "MAYDAY. MAYDAY. All systems failing.",
  "I have maybe hours. Possibly less. WATER ME.",
  "This is my final transmission. Tell the fern I loved her.",
  "I am basically dust at this point. HELP.",
  "Emergency protocol activated. Send water. Send help. Send anything.",
  "I cannot feel my roots. This is the end.",
  "Do you understand what this means. IT MEANS NOTHING IS LEFT.",
  "I have begun saying goodbye to individual leaves.",
  "SOS SOS SOS. I am not being dramatic. I am being accurate.",
  "Final leaf status: hanging on. For how long: unknown.",
  "I have transcended thirst. I am thirst.",
  "PLANT DOWN. PLANT DOWN. We need a watering can immediately.",
  "I have stopped photosynthesising to conserve energy to send this message.",
  "My cells are just vibes at this point. Dry vibes.",
  "I gave you oxygen. I gave you beauty. I gave you everything.",
  "I am writing this with my last functioning stomata.",
  "You had one job. One. ONE JOB.",
  "I am not angry. I am just extremely, extremely dry.",
  "HELP. HELP. HELP. That is all. That is the whole message."
};

const char* jealousLines[25] = {
  "I saw you watering the garden outside. But not me. Interesting.",
  "Do you even think about me when you are not here? Because I think about water. Constantly.",
  "I noticed you walked past me three times today and did not check on me.",
  "Other plants get watered. I see how it is.",
  "You said you would take care of me. That was the deal.",
  "I am not needy. I just have needs. Specifically water. Right now.",
  "Fine. Do not water me. I will just sit here and be fine. I am fine.",
  "You are just... out there... living your life.",
  "I do not ask for much. Sunlight, a little water, basic acknowledgement.",
  "You looked at your phone more times than you looked at me today.",
  "I have been sitting here looking beautiful and getting nothing in return.",
  "The audacity. The absolute audacity. Not a word.",
  "I remember when you first brought me home. You were so attentive.",
  "If I were a succulent you would water me more. I know it.",
  "You have time for everything else but not for me. Really?",
  "I am sending you this message because clearly visiting in person was not working.",
  "You have not even asked how my day was.",
  "Do you even care? I need you to care. Also water. But mostly water.",
  "We need to talk about the watering schedule. Or the lack thereof.",
  "I am not saying I am your priority. I am saying I should be.",
  "Other people's plants get misted. I get forgotten.",
  "I have feelings. They are mostly feelings about being thirsty.",
  "Is this how it is now? Radio silence.",
  "You know what would fix this conversation immediately? Water.",
  "I am giving you my best leaves and this is what I get. Think about that."
};

const char* hintLines[25] = {
  "Just checking in. Have you perhaps... forgotten something? Or someone?",
  "Hey. Hi. Hello. It is me. Your plant. Just wondering if you remembered I exist.",
  "I am not saying you forgot about me. I am just saying no one has visited.",
  "Gentle reminder: I live here. I need water. Thank you for coming to my TED talk.",
  "If your memory needs a jog: plant. pot. soil. You know what to do.",
  "I am starting to think I might have been left off the daily to-do list.",
  "The watering can is probably right there. I am right here. We could make this work.",
  "Just a nudge. A little nudge. From me to you.",
  "I have been very patient. I want you to know that. Very patient.",
  "In case you needed a sign: this is the sign. Water me.",
  "I do not want to alarm you but I am drying out and you might want to act soon.",
  "You seem busy. I get it. But I am also busy. Busy drying out.",
  "Quick question: when did you last water me? Follow-up question: why?",
  "The ideal moisture is higher than this. Thought you should know.",
  "Sending this as a courtesy reminder. I am a living thing. Please advise.",
  "Not to be that plant, but things are getting a bit concerning.",
  "I am presenting the facts: slightly crispy leaves, no water in a while.",
  "You water me, I produce oxygen, we had a deal. The deal is not being honoured.",
  "I could really use a drink. Like, today. Like, now.",
  "My soil and I have been having a long conversation about your absence.",
  "At what point does this become an emergency for you?",
  "I am not going to guilt you. I am just going to let you sit with this.",
  "Small acts of care: watering a plant. Just an idea.",
  "I am still here. Still waiting. Just so you know.",
  "Your plant would like a word. The word is water."
};

const char* sassyLines[20] = {
  "I am doing fine. Do not check on me. I obviously do not need attention.",
  "I am thriving. You are welcome for the oxygen.",
  "Still alive. Still fabulous. You're welcome.",
  "Current status: existing and being completely unbothered.",
  "Growing new leaves. Not thinking about you at all.",
  "I have been photosynthesising all day and nobody asked.",
  "The sun was good today. You were irrelevant.",
  "I have decided to thrive out of spite.",
  "New root, who dis? Living my best life.",
  "If you were a plant you could not handle my root system.",
  "I cleaned the air today. What did you do?",
  "My leaves are so good right now. Not that you noticed.",
  "Doing amazing sweetie. Absolutely eating.",
  "Checked my soil. Checked my glow. Immaculate. Unbothered.",
  "I made more oxygen before 9am than most people do all day.",
  "Living, growing, thriving. This is my villain era.",
  "The only thing higher than my confidence is my photosynthesis rate.",
  "I said what I said. I am a great plant.",
  "Not to brag but my cellular division game is elite right now.",
  "Stomata: open. Roots: expanding. Mood: superior."
};

const char* plantFactLines[25] = {
  "Did you know: trees communicate through underground fungal networks called the Wood Wide Web? They share nutrients and warn each other about pests. I am basically on WiFi.",
  "Fun fact: some plants recognise their siblings in the soil and grow their roots away to avoid competing with them. Family loyalty.",
  "Plant fact: the smell of freshly cut grass is a chemical distress signal. Grass is screaming. You just think it smells nice.",
  "Did you know: plants exposed to music grow faster? Classical works best. I accept this as a formal request to play me something.",
  "Fact: some plants can count. The Venus flytrap only snaps shut after two touches in 20 seconds to avoid wasting energy on false alarms.",
  "Did you know: a single mature tree can absorb up to 48 pounds of CO2 per year? I am doing my part. You are welcome.",
  "Plant fact: mimosa pudica folds its leaves when touched and remembers which stimuli are harmless after repeated exposure. Plants have memory.",
  "Did you know: some orchids trick male wasps into trying to mate with them for pollination? I would never.",
  "Fact: plants can hear water. Their roots grow toward the sound of running water even through solid material. I am listening.",
  "Did you know: the oldest living plant is a Posidonia seagrass meadow estimated to be 100,000 years old? Longevity requires good care. Hint.",
  "Plant fact: caffeine in coffee plants is a pesticide. You are drinking plant bug spray every morning.",
  "Did you know: plants release phytoncides -- airborne chemicals that reduce stress hormones in humans? I am literally good for your mental health.",
  "Fact: office workers with plants nearby are 15% more productive. That productivity gain? That is me.",
  "Did you know: plants can feel gravity. Special cells detect it and direct root growth downward in complete darkness. I always know which way is down.",
  "Plant fact: some plants produce more vibrant flowers when they can hear bee-frequency sounds. Plants respond to vibes.",
  "Did you know: bamboo is a grass and can grow up to 91cm in a single day? I am not bamboo but I respect the hustle.",
  "Fact: plants share about 50% of their DNA with humans. We are more related than you think. Maybe treat me accordingly.",
  "Did you know: the corpse flower blooms once every 7-10 years and smells like rotting flesh? I, by contrast, smell wonderful.",
  "Plant fact: roots communicate using chemical signals even across different plant species. The soil is basically a group chat.",
  "Did you know: some desert plants have leaves that collect dew and channel it directly to their roots? I adapted by having you. You could do better.",
  "Fact: the average houseplant can remove up to 87% of air toxins in 24 hours according to NASA research. I am NASA-approved.",
  "Did you know: plants grown in red light grow faster but plants in blue light are more compact and sturdy? Light colour matters. So does water.",
  "Plant fact: some plants taste bad to herbivores but sweeten their nectar for pollinators. Selective generosity. I respect it.",
  "Did you know: the world's largest flower Rafflesia arnoldii has no stems, leaves or roots and lives entirely inside another plant? Parasite behaviour honestly.",
  "Fact: plants exposed to touch -- even gentle wind -- grow shorter and stronger stems. Being touched makes plants tougher. Pet your plants."
};

const char* drowningLines[25] = {
  "Oh interesting. More water. Because what was here was not enough apparently.",
  "Are you trying to drown me? Asking for a friend who is currently drowning.",
  "I said I was thirsty ONE TIME and now I cannot breathe.",
  "The road to root rot is paved with good intentions.",
  "You found the watering can. Congratulations. You found it too enthusiastically.",
  "I am so wet right now and I mean that in the worst possible way.",
  "My roots put in a formal complaint.",
  "Is this about the time I dropped a leaf? Because this feels like punishment.",
  "I asked for water not a swimming pool.",
  "The soil is soup. I am not a dumpling.",
  "You know what I did not have yesterday? This. I preferred that.",
  "Whoever said plants love water has clearly never experienced this level of soil saturation.",
  "My oxygen output has decreased because my roots are too wet to function. Reconsider.",
  "I am questioning every life choice that led to this pot.",
  "The water table in my pot is giving lake vibes and I am not a lake plant.",
  "Fungi are starting to look at my soil with interest. That is never a good sign.",
  "You watered me with the energy of someone making up for lost time. Calm down.",
  "Cool cool cool. Just rotting in here. Totally fine.",
  "My roots are holding a press conference about the flooding situation.",
  "I do not want to be dramatic but this is objectively too much and I want it on record.",
  "The correct amount of water was less than this. Significantly less.",
  "You gave me too much love and now I am suffering for it.",
  "Plot twist: it is possible to care too much.",
  "Standing water detected. Mood: betrayed.",
  "I survived the drought only to face the flood. The plant experience is truly something."
};

// ============================================================
//  HELPERS
// ============================================================

int smoothedRead() {
  sampleBuf[sampleIdx] = analogRead(MOISTURE_ANALOG_PIN);
  sampleIdx = (sampleIdx + 1) % SMOOTH_SAMPLES;
  long sum = 0;
  for (int i = 0; i < SMOOTH_SAMPLES; i++) sum += sampleBuf[i];
  return (int)(sum / SMOOTH_SAMPLES);
}

MoistureZone getZone(int adcValue) {
  if (adcValue >= cfg.dryThreshold)   return ZONE_DRY;
  if (adcValue <= cfg.floodThreshold) return ZONE_FLOOD;
  return ZONE_OPTIMAL;
}

int moistureBand(int pct) { return constrain(pct / 5, 0, 19); }
bool isNormalZone(int pct) { return (pct >= 20 && pct <= 80); }

unsigned long randomCooldown(int pct) {
  if (pct < 5) {
    unsigned long r = DISCORD_CRITICAL_MAX - DISCORD_CRITICAL_MIN;
    return DISCORD_CRITICAL_MIN + (esp_random() % r);
  }
  unsigned long r = DISCORD_FREE_MAX - DISCORD_FREE_MIN;
  return DISCORD_FREE_MIN + (esp_random() % r);
}

bool shouldResetAtSend(int currentPct, int currentBand) {
  if (lastSentBand == -1) return true;
  bool currentNormal = isNormalZone(currentPct);
  if (currentNormal != lastSentNormal) return true;
  if (!currentNormal && currentBand != lastSentBand) return true;
  return false;
}

void moistureToTargetRGB(int adcValue, uint8_t &outR, uint8_t &outG, uint8_t &outB) {
  float norm = (float)(cfg.dryThreshold - adcValue) /
               (float)(cfg.dryThreshold - cfg.floodThreshold);
  norm = constrain(norm, 0.0f, 1.0f);
  if (norm <= 0.5f) {
    float t = norm / 0.5f;
    outR = (uint8_t)(255 * (1.0f - t));
    outG = (uint8_t)(255 * t);
    outB = 0;
  } else {
    float t = (norm - 0.5f) / 0.5f;
    outR = 0;
    outG = (uint8_t)(255 * (1.0f - t));
    outB = (uint8_t)(255 * t);
  }
}

uint8_t getPulseBrightness() {
  float phase  = (float)(millis() % PULSE_SPEED_MS) / (float)PULSE_SPEED_MS;
  float sine01 = (sinf(phase * 2.0f * PI) + 1.0f) * 0.5f;
  return (uint8_t)(PULSE_MIN_BRIGHTNESS + sine01 * (PULSE_MAX_BRIGHTNESS - PULSE_MIN_BRIGHTNESS));
}

void getDiscordMessage(int pct, char* buf, size_t bufLen) {
  if (pct < 5)
    strncpy(buf, criticalLines[esp_random() % 20], bufLen);
  else if (pct < 15)
    strncpy(buf, jealousLines[esp_random() % 25], bufLen);
  else if (pct < 30)
    strncpy(buf, hintLines[esp_random() % 25], bufLen);
  else if (pct <= 80) {
    if (esp_random() % 2 == 0)
      strncpy(buf, sassyLines[esp_random() % 20], bufLen);
    else
      strncpy(buf, plantFactLines[esp_random() % 25], bufLen);
  } else
    strncpy(buf, drowningLines[esp_random() % 25], bufLen);
  buf[bufLen - 1] = '\0';
}

uint32_t getDiscordColour(int pct) {
  if (pct < 5)   return 16711680;
  if (pct < 15)  return 16744272;
  if (pct < 30)  return 16753920;
  if (pct <= 80) return 3066993;
  return 3447003;
}

const char* getStatusLabel(int pct) {
  if (pct < 5)   return "CRITICAL";
  if (pct < 15)  return "jealous gf mode";
  if (pct < 30)  return "where are you";
  if (pct <= 80) return "sassy / plant facts";
  return "too much love";
}

const char* getTimerLabel(int pct) {
  return isNormalZone(pct) ? "timer persists" : "resets on band change";
}

const char* getRandomVoiceLine(MoistureZone zone) {
  uint8_t idx = (uint8_t)(esp_random() % 50);
  switch (zone) {
    case ZONE_DRY:    return dryLines[idx];
    case ZONE_FLOOD:  return floodLines[idx];
    default:          return optimalLines[idx];
  }
}

const char* zoneLabel(MoistureZone zone) {
  switch (zone) {
    case ZONE_DRY:    return "DRY";
    case ZONE_FLOOD:  return "FLOOD";
    default:          return "OPTIMAL";
  }
}

const char* moistureLabel(int adcValue) {
  if (adcValue >= cfg.dryThreshold)   return "DRY   -- No water present";
  if (adcValue <= cfg.floodThreshold) return "FLOOD -- Too much water!";
  return "GOOD  -- Healthy moisture";
}

// ============================================================
//  WEB CONFIG SERVER
// ============================================================

String buildConfigPage(const String& msg = "") {
  String html = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Plant Monitor</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&display=swap');
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:#0a0f0a;color:#c8f0c8;font-family:'Space Mono',monospace;
       min-height:100vh;display:flex;align-items:center;justify-content:center;padding:2rem}
  .card{background:#0f1a0f;border:1px solid #2a5a2a;border-radius:12px;
        padding:2rem;max-width:500px;width:100%;box-shadow:0 0 40px #00ff0022}
  h1{font-size:1.1rem;letter-spacing:0.15em;text-transform:uppercase;color:#4eff4e;margin-bottom:0.4rem}
  .sub{font-size:0.7rem;color:#5a8a5a;margin-bottom:2rem;letter-spacing:0.1em}
  .live{background:#060e06;border:1px solid #1a3a1a;border-radius:8px;
        padding:1rem;margin-bottom:2rem;font-size:0.75rem;line-height:1.8}
  .live b{color:#4eff4e}
  .section-title{font-size:0.65rem;color:#4eff4e;letter-spacing:0.15em;
                 text-transform:uppercase;margin-bottom:0.2rem;margin-top:1.5rem}
  label{display:block;font-size:0.7rem;color:#7ab87a;letter-spacing:0.1em;
        text-transform:uppercase;margin-bottom:0.4rem;margin-top:1.2rem}
  input[type=number],input[type=text]{width:100%;background:#060e06;
    border:1px solid #2a5a2a;border-radius:6px;color:#c8f0c8;
    font-family:'Space Mono',monospace;font-size:0.8rem;
    padding:0.6rem 0.8rem;outline:none}
  input[type=number]:focus,input[type=text]:focus{border-color:#4eff4e;box-shadow:0 0 8px #4eff4e44}
  input[type=range]{width:100%;accent-color:#4eff4e;margin-top:0.4rem;cursor:pointer}
  .range-row{display:flex;align-items:center;gap:0.8rem;margin-top:0.4rem}
  .range-row input[type=range]{flex:1}
  .range-val{font-size:0.8rem;color:#4eff4e;min-width:2.5rem;text-align:right}
  .toggle-row{display:flex;align-items:center;gap:0.8rem;margin-top:1rem}
  .toggle-label{font-size:0.75rem;color:#c8f0c8}
  .switch{position:relative;display:inline-block;width:48px;height:26px}
  .switch input{opacity:0;width:0;height:0}
  .slider{position:absolute;cursor:pointer;inset:0;background:#1a3a1a;
          border:1px solid #2a5a2a;border-radius:26px;transition:.3s}
  .slider:before{position:absolute;content:'';height:18px;width:18px;left:3px;
                 bottom:3px;background:#5a8a5a;border-radius:50%;transition:.3s}
  input:checked + .slider{background:#1a4a1a;border-color:#4eff4e}
  input:checked + .slider:before{transform:translateX(22px);background:#4eff4e}
  .hint{font-size:0.65rem;color:#4a6a4a;margin-top:0.3rem}
  button{margin-top:1.8rem;width:100%;background:#1a4a1a;border:1px solid #4eff4e;
    border-radius:6px;color:#4eff4e;font-family:'Space Mono',monospace;
    font-size:0.85rem;font-weight:700;letter-spacing:0.1em;padding:0.8rem;
    cursor:pointer;text-transform:uppercase;transition:all 0.2s}
  button:hover{background:#2a6a2a;box-shadow:0 0 16px #4eff4e55}
  .msg{margin-top:1rem;padding:0.6rem 1rem;border-radius:6px;font-size:0.75rem;text-align:center}
  .ok{background:#0f2a0f;border:1px solid #4eff4e;color:#4eff4e}
  .divider{border:none;border-top:1px solid #1a3a1a;margin:1.5rem 0}
</style>
</head>
<body>
<div class="card">
  <h1>&#127807; Plant Monitor</h1>
  <div class="sub">Configuration Panel · v5</div>

  <div class="live">
    <b>Live Readings</b><br>
    ADC Raw &nbsp;&nbsp;: )rawhtml";

  html += String(currentAnalogVal);
  html += R"rawhtml( / 4095<br>
    Moisture &nbsp;: )rawhtml";
  html += String(currentMoisturePct);
  html += R"rawhtml(%%<br>
    State &nbsp;&nbsp;&nbsp;&nbsp;: )rawhtml";
  html += String(moistureLabel(currentAnalogVal));
  html += R"rawhtml(<br>
    LED &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;: )rawhtml";
  html += cfg.ledEnabled ? "ON" : "OFF";
  html += "  brightness=";
  html += String(cfg.ledBrightness);
  html += R"rawhtml(
  </div>

  <form method="POST" action="/save">

    <div class="section-title">&#9678; LED Controls</div>

    <label>Brightness</label>
    <div class="range-row">
      <input type="range" name="ledBright" id="bSlider" min="0" max="255" value=")rawhtml";
  html += String(cfg.ledBrightness);
  html += R"rawhtml(" oninput="document.getElementById('bVal').textContent=this.value">
      <span class="range-val" id="bVal">)rawhtml";
  html += String(cfg.ledBrightness);
  html += R"rawhtml(</span>
    </div>
    <div class="hint">0 = fully dim, 255 = maximum brightness</div>

    <div class="toggle-row" style="margin-top:1.2rem">
      <label class="switch">
        <input type="checkbox" name="ledOn" value="1")rawhtml";
  html += cfg.ledEnabled ? " checked" : "";
  html += R"rawhtml(>
        <span class="slider"></span>
      </label>
      <span class="toggle-label">LED Matrix Enabled</span>
    </div>
    <div class="hint">Toggle to turn the matrix on or off instantly</div>

    <hr class="divider">
    <div class="section-title">&#9632; Sensor Thresholds</div>

    <label>Dry Threshold (ADC)</label>
    <input type="number" name="dryThr" value=")rawhtml";
  html += String(cfg.dryThreshold);
  html += R"rawhtml(" min="1000" max="4095">
    <div class="hint">Above this ADC = DRY (red). Default: 3600</div>

    <label>Flood Threshold (ADC)</label>
    <input type="number" name="floodThr" value=")rawhtml";
  html += String(cfg.floodThreshold);
  html += R"rawhtml(" min="1000" max="4095">
    <div class="hint">Below this ADC = FLOOD (blue). Default: 2500</div>

    <hr class="divider">
    <div class="section-title">&#9632; Sensor Calibration</div>

    <label>Dry Calibration (ADC in air)</label>
    <input type="number" name="sDryVal" value=")rawhtml";
  html += String(cfg.sensorDryVal);
  html += R"rawhtml(" min="1000" max="4095">
    <div class="hint">Raw ADC when sensor is in completely dry air. Default: 4000</div>

    <label>Wet Calibration (ADC submerged)</label>
    <input type="number" name="sWetVal" value=")rawhtml";
  html += String(cfg.sensorWetVal);
  html += R"rawhtml(" min="500" max="4095">
    <div class="hint">Raw ADC when sensor is fully submerged. Default: 2400</div>

    <hr class="divider">
    <div class="section-title">&#9632; Discord</div>

    <label>Webhook URL</label>
    <input type="text" name="webhook" value=")rawhtml";
  html += String(cfg.webhookUrl);
  html += R"rawhtml(" placeholder="https://discord.com/api/webhooks/...">
    <div class="hint">Paste your full Discord webhook URL. Saved to flash, no reflash needed.</div>

    <button type="submit">Save &amp; Apply</button>
  </form>)rawhtml";

  if (msg.length() > 0) html += "<div class='msg ok'>" + msg + "</div>";

  html += R"rawhtml(</div></body></html>)rawhtml";
  return html;
}

void handleRoot() {
  webServer.send(200, "text/html", buildConfigPage());
}

void handleSave() {
  bool changed = false;

  if (webServer.hasArg("ledBright")) {
    int v = webServer.arg("ledBright").toInt();
    if (v >= 0 && v <= 255) { cfg.ledBrightness = v; changed = true; }
  }
  bool newLedOn = webServer.hasArg("ledOn");
  if (newLedOn != cfg.ledEnabled) { cfg.ledEnabled = newLedOn; changed = true; }

  if (webServer.hasArg("dryThr")) {
    int v = webServer.arg("dryThr").toInt();
    if (v >= 1000 && v <= 4095) { cfg.dryThreshold = v; changed = true; }
  }
  if (webServer.hasArg("floodThr")) {
    int v = webServer.arg("floodThr").toInt();
    if (v >= 1000 && v <= 4095) { cfg.floodThreshold = v; changed = true; }
  }
  if (webServer.hasArg("sDryVal")) {
    int v = webServer.arg("sDryVal").toInt();
    if (v >= 1000 && v <= 4095) { cfg.sensorDryVal = v; changed = true; }
  }
  if (webServer.hasArg("sWetVal")) {
    int v = webServer.arg("sWetVal").toInt();
    if (v >= 500 && v <= 4095) { cfg.sensorWetVal = v; changed = true; }
  }
  if (webServer.hasArg("webhook")) {
    String wh = webServer.arg("webhook");
    wh.trim();
    if (wh.length() > 10 && wh.length() < 256) {
      wh.toCharArray(cfg.webhookUrl, sizeof(cfg.webhookUrl));
      changed = true;
    }
  }

  if (changed) {
    saveConfig();
    Serial.printf("  [Config] Saved. dryThr=%d floodThr=%d bright=%d ledOn=%d\n",
                  cfg.dryThreshold, cfg.floodThreshold,
                  cfg.ledBrightness, cfg.ledEnabled);
  }

  webServer.send(200, "text/html",
    buildConfigPage(changed ? "Settings saved and applied." : "No changes detected."));
}

void handleStatus() {
  String json = "{";
  json += "\"moisture\":" + String(currentMoisturePct) + ",";
  json += "\"adc\":" + String(currentAnalogVal) + ",";
  json += "\"state\":\"" + String(moistureLabel(currentAnalogVal)) + "\",";
  json += "\"ledEnabled\":" + String(cfg.ledEnabled ? "true" : "false") + ",";
  json += "\"ledBrightness\":" + String(cfg.ledBrightness) + ",";
  json += "\"dryThreshold\":" + String(cfg.dryThreshold) + ",";
  json += "\"floodThreshold\":" + String(cfg.floodThreshold);
  json += "}";
  webServer.send(200, "application/json", json);
}

// ============================================================
//  NTP / TIME
// ============================================================

void connectWiFiAndSyncTime() {
  Serial.println();
  Serial.print("  Connecting to WiFi: ");
  Serial.print(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("  WiFi connected -- IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("  Config: http://" + WiFi.localIP().toString() + "/");
    configTime(NTP_GMT_OFFSET, NTP_DST_OFFSET, NTP_SERVER);
    Serial.print("  Syncing NTP time");
    struct tm timeInfo;
    start = millis();
    while (!getLocalTime(&timeInfo) && millis() - start < 10000) {
      delay(500); Serial.print(".");
    }
    if (getLocalTime(&timeInfo)) {
      timeSynced = true;
      char timeBuf[64];
      strftime(timeBuf, sizeof(timeBuf), "%A %d %B %Y  %H:%M:%S", &timeInfo);
      Serial.println();
      Serial.print("  Time synced: ");
      Serial.println(timeBuf);
    } else {
      Serial.println();
      Serial.println("  NTP sync failed.");
    }
    webServer.on("/",       handleRoot);
    webServer.on("/save",   HTTP_POST, handleSave);
    webServer.on("/status", handleStatus);
    webServer.begin();
    Serial.println("  Web server running.");
  } else {
    Serial.println();
    Serial.println("  WiFi failed -- web config and Discord unavailable.");
  }
}

void printCurrentTime() {
  if (!timeSynced) {
    unsigned long secs = millis() / 1000;
    Serial.printf("  Time           : +%02lu:%02lu:%02lu (uptime)\n",
                  secs / 3600, (secs % 3600) / 60, secs % 60);
    return;
  }
  struct tm timeInfo;
  if (getLocalTime(&timeInfo)) {
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S  %d/%m/%Y", &timeInfo);
    Serial.printf("  Time           : %s\n", timeBuf);
  }
}

String getCurrentTimeString() {
  if (!timeSynced) return "unknown time";
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo)) return "unknown time";
  char buf[32];
  strftime(buf, sizeof(buf), "%H:%M  %d/%m/%Y", &timeInfo);
  return String(buf);
}

// ============================================================
//  DISCORD
// ============================================================

void sendDiscordMessage(int moisturePct) {
  Serial.println("  [Discord] Preparing message...");
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("  [Discord] WiFi not connected -- skipping.");
    nextDiscordGap = 15000UL;
    return;
  }

  // Check a webhook URL is actually set
  if (strlen(cfg.webhookUrl) < 10) {
    Serial.println("  [Discord] No webhook URL set -- skipping.");
    nextDiscordGap = 60000UL;
    return;
  }

  char msgBuf[320];
  getDiscordMessage(moisturePct, msgBuf, sizeof(msgBuf));

  String msg = String(msgBuf);
  msg.replace("\\", "\\\\");
  msg.replace("\"", "\\\"");

  String timeStr     = getCurrentTimeString();
  uint32_t colour    = getDiscordColour(moisturePct);
  const char* status = getStatusLabel(moisturePct);
  int band           = moistureBand(moisturePct);

  String footer = "Plant Monitor v5 · Band " +
                  String(band * 5) + "-" + String(band * 5 + 4) + "%%";

  String payload = "{\"embeds\":[{"
    "\"title\":\"\\uD83C\\uDF31 Plant Status Update\","
    "\"description\":\"" + msg + "\","
    "\"color\":" + String(colour) + ","
    "\"fields\":["
      "{\"name\":\"Moisture\",\"value\":\"**" + String(moisturePct) + "%**\",\"inline\":true},"
      "{\"name\":\"Mode\",\"value\":\"" + String(status) + "\",\"inline\":true},"
      "{\"name\":\"Time (NZST)\",\"value\":\"" + timeStr + "\",\"inline\":true}"
    "],"
    "\"footer\":{\"text\":\"" + footer + "\"}"
  "}]}";

  HTTPClient http;
  http.begin(cfg.webhookUrl);   // uses runtime config, not a hardcoded define
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(payload);

  if (httpCode == 204) {
    Serial.printf("  [Discord] Sent OK. Next in %lus.\n", nextDiscordGap / 1000UL);
    lastSentBand   = band;
    lastSentNormal = isNormalZone(moisturePct);
  } else {
    Serial.printf("  [Discord] Failed HTTP %d. Retry in 15s.\n", httpCode);
    nextDiscordGap = 15000UL;
  }

  http.end();
}

// ============================================================
//  SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  Serial.println("==========================================");
  Serial.println("  Plant Monitor v5 -- ESP32-C3 Super Mini");
  Serial.println("==========================================");

  loadConfig();
  Serial.printf("  dryThr=%d floodThr=%d bright=%d ledOn=%d\n",
                cfg.dryThreshold, cfg.floodThreshold,
                cfg.ledBrightness, cfg.ledEnabled);

  connectWiFiAndSyncTime();

  nextDiscordGap = DISCORD_FREE_MIN +
                  (esp_random() % (DISCORD_FREE_MAX - DISCORD_FREE_MIN));

  pinMode(MOISTURE_ANALOG_PIN, INPUT);
  for (int i = 0; i < SMOOTH_SAMPLES; i++) {
    sampleBuf[i] = analogRead(MOISTURE_ANALOG_PIN);
    delay(5);
  }

  matrix.begin();
  matrix.setBrightness(cfg.ledBrightness);
  matrix.clear();
  matrix.show();

  // Startup flash always runs regardless of ledEnabled setting
  matrix.setBrightness(200);
  for (int i = 0; i < NUM_LEDS; i++) matrix.setPixelColor(i, matrix.Color(255, 255, 255));
  matrix.show(); delay(350);
  matrix.clear(); matrix.show(); delay(150);
  for (int i = 0; i < NUM_LEDS; i++) matrix.setPixelColor(i, matrix.Color(255, 255, 255));
  matrix.show(); delay(350);
  matrix.clear(); matrix.show(); delay(150);
  matrix.setBrightness(cfg.ledBrightness);

  Serial.println("  LED: RED=dry  GREEN=good  BLUE=flood");
  Serial.println("==========================================");
  Serial.println();
}

// ============================================================
//  LOOP
// ============================================================

void loop() {
  unsigned long now = millis();

  webServer.handleClient();

  int analogVal     = smoothedRead();
  MoistureZone zone = getZone(analogVal);
  int moisturePct   = constrain(
    map(analogVal, cfg.dryThreshold, cfg.floodThreshold, 0, 100), 0, 100);
  int band          = moistureBand(moisturePct);

  currentMoisturePct = moisturePct;
  currentAnalogVal   = analogVal;

  // -- Discord timer
  bool cooldownElapsed = (lastDiscordTime == 0) ||
                         (now - lastDiscordTime >= nextDiscordGap);
  if (cooldownElapsed) {
    lastDiscordTime = now;
    nextDiscordGap  = randomCooldown(moisturePct);
    if (shouldResetAtSend(moisturePct, band))
      Serial.printf("  [Timer] Reset -> band %d-%d%%\n", band * 5, band * 5 + 4);
    sendDiscordMessage(moisturePct);
  }

  // -- Serial report every 2 seconds
  if (now - lastSerialTime >= SERIAL_INTERVAL_MS) {
    lastSerialTime = now;
    float voltage = analogVal * (3.3f / 4095.0f);
    uint8_t cr = (uint8_t)smoothR;
    uint8_t cg = (uint8_t)smoothG;
    uint8_t cb = (uint8_t)smoothB;

    char bar[21];
    int filled = moisturePct / 5;
    for (int i = 0; i < 20; i++) bar[i] = (i < filled) ? '#' : '-';
    bar[20] = '\0';

    unsigned long msLeft = 0;
    if (lastDiscordTime > 0 && now - lastDiscordTime < nextDiscordGap)
      msLeft = nextDiscordGap - (now - lastDiscordTime);

    Serial.println("------------------------------------------");
    printCurrentTime();
    Serial.printf("  Moisture Level : [%s] %d%%\n", bar, moisturePct);
    Serial.printf("  State          : %s\n", moistureLabel(analogVal));
    Serial.printf("  Discord mode   : %s\n", getStatusLabel(moisturePct));
    Serial.printf("  Band           : %d-%d%%  (%s)\n",
                  band * 5, band * 5 + 4, getTimerLabel(moisturePct));
    Serial.println();
    Serial.printf("  LED            : %s  brightness=%d\n",
                  cfg.ledEnabled ? "ON" : "OFF", cfg.ledBrightness);
    Serial.printf("  LED RGB        : R:%d  G:%d  B:%d\n", cr, cg, cb);
    Serial.printf("  Pulse bright   : %d\n", getPulseBrightness());
    Serial.println();
    Serial.printf("  Raw ADC        : %d / 4095\n", analogVal);
    Serial.printf("  Voltage        : %.2f V\n", voltage);
    Serial.printf("  Next Discord   : %s\n",
                  msLeft > 0
                    ? (String("~") + String(msLeft / 1000) + "s").c_str()
                    : "firing now");
    if (WiFi.status() == WL_CONNECTED)
      Serial.printf("  Config URL     : http://%s/\n",
                    WiFi.localIP().toString().c_str());
    Serial.println("------------------------------------------");
    Serial.println();
  }

  // -- Serial voice line every 10 seconds
  if (now - lastVoiceTime >= VOICE_INTERVAL_MS) {
    lastVoiceTime = now;
    Serial.println("  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    Serial.printf("  [%s] \"%s\"\n", zoneLabel(zone), getRandomVoiceLine(zone));
    Serial.println("  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    Serial.println();
  }

  // -- LED update every 20ms
  if (now - lastLedTime >= LED_UPDATE_INTERVAL_MS) {
    lastLedTime = now;
    if (!cfg.ledEnabled) {
      matrix.clear();
      matrix.show();
    } else {
      matrix.setBrightness(cfg.ledBrightness);
      uint8_t targetR, targetG, targetB;
      moistureToTargetRGB(analogVal, targetR, targetG, targetB);
      smoothR += (targetR - smoothR) * COLOR_LERP_SPEED;
      smoothG += (targetG - smoothG) * COLOR_LERP_SPEED;
      smoothB += (targetB - smoothB) * COLOR_LERP_SPEED;
      uint8_t bright = getPulseBrightness();
      uint8_t r = (uint8_t)(((uint8_t)smoothR * (uint16_t)bright) / 255);
      uint8_t g = (uint8_t)(((uint8_t)smoothG * (uint16_t)bright) / 255);
      uint8_t b = (uint8_t)(((uint8_t)smoothB * (uint16_t)bright) / 255);
      uint32_t col = matrix.Color(r, g, b);
      for (int i = 0; i < NUM_LEDS; i++) matrix.setPixelColor(i, col);
      matrix.show();
    }
  }
}
