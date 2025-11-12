// ATtiny45 (or ATtiny85) + SK6812 RGBW (tinyNeoPixel)
// Tools (ATTinyCore): Clock 8 MHz (internal), millis()/micros(): Disabled, LTO: Enabled

#include <Arduino.h>
#include <avr/power.h>
#include <avr/pgmspace.h>
#include <tinyNeoPixel.h>

#define LED_PIN    0
#define LED_COUNT  16
#define LED_TYPE   NEO_GRBW
#define BRIGHT     240  // (0..255) global max; per-scene gain modulates further

tinyNeoPixel strip(LED_COUNT, LED_PIN, LED_TYPE);

struct Rgbw { uint8_t r,g,b,w; };

// ---------- tiny utils ----------
static uint16_t rng16 = 0xACE1u;
static inline uint8_t rnd8(){
  // xorshift-16, very small but better period than 8-bit
  rng16 ^= rng16 << 7;
  rng16 ^= rng16 >> 9;
  rng16 ^= rng16 << 8;
  return (uint8_t)rng16;
}

static inline uint8_t scale8(uint8_t v,uint8_t s){ return ((uint16_t)v*s)>>8; }
static inline uint8_t clampu(int16_t v){ if(v<0) return 0; if(v>255) return 255; return (uint8_t)v; }
static inline uint8_t video8(uint8_t v){ return (uint16_t)v*(v+64)/255; }
#define MUL8(a,b) ((uint8_t)(((uint16_t)(a)*(uint16_t)(b))>>8))

// Per-scene gain (0..255). Applied on top of BRIGHT.
static uint8_t sceneGain = 255;
static uint8_t lastScene1 = 255; // previous
static uint8_t lastScene2 = 255; // one before previous

// Render one pixel (no hard RGB clamp; respects sceneGain)
static inline void setPix(uint8_t i,const Rgbw& c){
  const uint8_t gb = MUL8(BRIGHT, sceneGain);
  strip.setPixelColor(i,
    scale8(c.r, gb),
    scale8(c.g, gb),
    scale8(c.b, gb),
    scale8(c.w, gb)
  );
}

// Small flicker
static inline void flicker(Rgbw &c,uint8_t a){
  uint8_t f=rnd8() & a; if(c.w>f) c.w-=f;
  c.r = clampu((int16_t)c.r + ((int8_t)(rnd8()&3) - 1));
  c.g = clampu((int16_t)c.g + ((int8_t)(rnd8()&3) - 1));
  c.b = clampu((int16_t)c.b + ((int8_t)(rnd8()&3) - 1));
}

// ---------- Palettes in PROGMEM (compile-time gamma) ----------
#define V8(x)   ((uint8_t)(((uint16_t)(x)*((x)+64))/255))
#define PAL(_r,_g,_b,_w) { V8(_r), V8(_g), V8(_b), V8(_w) }

const uint8_t PROGMEM PAL_WARM[4]    = PAL(95,  60, 22, 110);
const uint8_t PROGMEM PAL_NEUT[4]    = PAL(28,  28, 44, 110);
const uint8_t PROGMEM PAL_COOL[4]    = PAL(12,  24, 190, 45);
const uint8_t PROGMEM PAL_DARK[4]    = PAL(10,  10, 16,  0);
const uint8_t PROGMEM PAL_AD[4]      = PAL(220, 220, 220, 80);
const uint8_t PROGMEM PAL_NEWSB[4]   = PAL(20,  40, 200, 20);
const uint8_t PROGMEM PAL_AMBER[4]   = PAL(220, 180, 10, 40);
const uint8_t PROGMEM PAL_MAGENTA[4] = PAL(200, 20, 200, 30);
// new: green/yellow field tone
const uint8_t PROGMEM PAL_FIELD[4]   = PAL(30, 180, 40, 25);

// Read one channel from a PROGMEM palette
static inline uint8_t palCh(const uint8_t *pal, uint8_t idx){
  return pgm_read_byte(&pal[idx]);
}

// Blend two PROGMEM palettes at factor t (0..255) into Rgbw
static inline Rgbw mixPal(const uint8_t *A, const uint8_t *B, uint8_t t){
  Rgbw c;
  uint8_t ar=palCh(A,0), ag=palCh(A,1), ab=palCh(A,2), aw=palCh(A,3);
  uint8_t br=palCh(B,0), bg=palCh(B,1), bb=palCh(B,2), bw=palCh(B,3);
  c.r = ar + (((int16_t)br - ar)*t >> 8);
  c.g = ag + (((int16_t)bg - ag)*t >> 8);
  c.b = ab + (((int16_t)bb - ab)*t >> 8);
  c.w = aw + (((int16_t)bw - aw)*t >> 8);
  return c;
}

// Dim a PROGMEM palette by s (0..255)
static inline Rgbw dimPal(const uint8_t *A, uint8_t s){
  Rgbw c;
  c.r = scale8(palCh(A,0), s);
  c.g = scale8(palCh(A,1), s);
  c.b = scale8(palCh(A,2), s);
  c.w = scale8(palCh(A,3), s);
  return c;
}

// ---------- Scene engine ----------
enum Scene {
  SC_WARMFADE, SC_COOLSWEEP, SC_DARKCUTS, SC_TICKER, SC_ADSPULSE, SC_CINEMAWIPE,
  SC_AMBERGLOW, SC_MAGENTADRAMA, SC_SPORTSFIELD, SC_SUNSETFADE, SC__COUNT
};
static Scene scene = SC_WARMFADE, lastScene = SC__COUNT;
static uint16_t sceneMs = 3000;
static uint8_t patchLen=4, patchPos=0, sweepPos=0, tickerPos=0, wipePos=0, wipeDir=1,sunsetPhase = 0;

static uint8_t rnd01(){ return rnd8()&1; }
static uint16_t randomSceneMs(bool isAds){
  // randomness + occasional extra hold
  uint16_t s = 1600 + (uint16_t)rnd8()*12 + (uint16_t)rnd8()*9 + (uint16_t)rnd8()*7; // ~1.6–7.9s
  if(isAds) s += 1200 + (rnd8()*4);
  if((rnd8()&3)==0) s += 900 + (rnd8()*3); // 25% extend
  return s;
}

static void startScene(){
  uint8_t pick;
  do {
    pick = rnd8()%21; // was %20
         // weights: warm3, cool3, ticker2, dark2, ads3, wipe2, amber3, magenta2, field2, sunset1
    if      (pick<3)   scene=SC_WARMFADE;
    else if (pick<6)   scene=SC_COOLSWEEP;
    else if (pick<8)   scene=SC_TICKER;
    else if (pick<10)  scene=SC_DARKCUTS;
    else if (pick<13)  scene=SC_ADSPULSE;
    else if (pick<15)  scene=SC_CINEMAWIPE;
    else if (pick<18)  scene=SC_AMBERGLOW;
    else if (pick<20)  scene=SC_MAGENTADRAMA;
    else               scene=SC_SUNSETFADE;
  } while(scene==lastScene);

  // per-scene brightness “gain” (on top of BRIGHT)
  sceneGain = 255; // default
  if(scene==SC_DARKCUTS)      sceneGain = 200;
  else if(scene==SC_TICKER)   sceneGain = 230;
  else if(scene==SC_COOLSWEEP)sceneGain = 255;
  else if(scene==SC_ADSPULSE) sceneGain = 255;
  else if(scene==SC_CINEMAWIPE)sceneGain= 230;
  else if(scene==SC_SPORTSFIELD)sceneGain= 240;

  sceneMs  = randomSceneMs(scene==SC_ADSPULSE);
  patchLen = 3 + (rnd8()&3);
  patchPos = 0;
  sweepPos = rnd8()%LED_COUNT;
  tickerPos= rnd8()%LED_COUNT;
  wipePos  = rnd01()?0:(LED_COUNT-1);
  wipeDir  = rnd01()?1:255;
  sunsetPhase = rnd8();
  lastScene= scene;
}

// ---------- Scene renderers ----------
static void renderBlend(const uint8_t *A, const uint8_t *B, uint8_t flickAmt){
  const uint8_t *a=A, *b=B;
  for(uint8_t i=0;i<LED_COUNT;i++){
    if(patchPos>=patchLen){ patchPos=0; patchLen=3+(rnd8()&3); const uint8_t *t=a; a=b; b=t; }
    uint8_t t = (rnd8()&31)<<3;
    Rgbw c = mixPal(a,b,t); flicker(c,flickAmt); setPix(i,c); patchPos++;
  }
}
static void renderWarmFade(){     renderBlend(PAL_WARM,    PAL_NEUT,   0x0A); } // slightly gentler flicker
static void renderAmberGlow(){    renderBlend(PAL_AMBER,   PAL_NEUT,   0x08); }
static void renderMagentaDrama(){ renderBlend(PAL_MAGENTA, PAL_NEUT,   0x0C); }

static void renderCoolSweep(){ // brighter, clearer highlight
  Rgbw base = dimPal(PAL_COOL, 255);
  for(uint8_t i=0;i<LED_COUNT;i++){
    Rgbw c=base;
    if(i==sweepPos || i==(uint8_t)(sweepPos+1)){
      c.b = clampu(c.b + 110);
      c.w = clampu(c.w + 50);
    }
    flicker(c,0x12); setPix(i,c);
  }
  if(rnd8()&1) sweepPos=(uint8_t)(sweepPos+1)%LED_COUNT;
}

static void renderDarkCuts(){
  Rgbw base = dimPal(PAL_DARK, 200);
  for(uint8_t i=0;i<LED_COUNT;i++){
    Rgbw c=base;
    if((rnd8()&31)==0){ c = mixPal(PAL_DARK, PAL_NEUT, 230); c.w=clampu(c.w+20); }
    flicker(c,0x06); setPix(i,c);
  }
}

static void renderTicker(){
  const uint8_t band=3;
  for(uint8_t i=0;i<LED_COUNT;i++){
    Rgbw c = dimPal(PAL_NEUT, 255);
    if(i>=tickerPos && i<(uint8_t)(tickerPos+band)){
      c = mixPal(PAL_NEUT, PAL_NEWSB, 240); c.w=clampu(c.w+16);
    }
    flicker(c,0x0A); setPix(i,c);
  }
  if(!(rnd8()&1)){ tickerPos++; if(tickerPos>=LED_COUNT) tickerPos=0; }
}

static void renderAdsPulse(){
  Rgbw base = mixPal(PAL_AD, (rnd8()&1)?PAL_COOL:PAL_WARM, 140);
  for(uint8_t i=0;i<LED_COUNT;i++){
    Rgbw c=base;
    if((rnd8()&5)==0){ c.r=clampu(c.r+45); c.g=clampu(c.g+45); c.b=clampu(c.b+45); c.w=clampu(c.w+70); }
    flicker(c,0x12); setPix(i,c);
  }
}

static void renderCinemaWipe(){
  for(uint8_t i=0;i<LED_COUNT;i++){
    Rgbw c = dimPal(PAL_NEUT, 255);
    if(i==wipePos) c=mixPal(PAL_NEUT,PAL_WARM,240);
    else {
      int8_t d=(int8_t)i - (int8_t)wipePos;
      if(wipeDir==255) d = -d;
      if(d<0) d = (int8_t)(d + LED_COUNT);
      if(d>=0 && d<4){ uint8_t t=(uint8_t)(200 - d*50); c=mixPal(PAL_NEUT,PAL_WARM,t); }
    }
    flicker(c,0x09); setPix(i,c);
  }
  wipePos = (uint8_t)(wipePos + wipeDir);
  if(wipePos >= LED_COUNT) wipePos = (wipeDir==1)?0:(LED_COUNT-1);
}

// SportsField (green band moving; occasional bright “stadium” pops)
static void renderSportsField(){
  const uint8_t band = 4;
  uint8_t head = (uint8_t)(sweepPos % LED_COUNT);
  for(uint8_t i=0;i<LED_COUNT;i++){
    // base slightly neutral so whites read correctly
    Rgbw c = mixPal(PAL_NEUT, PAL_FIELD, 200);
    // moving field band
    uint8_t inBand = ( (i - head + LED_COUNT) % LED_COUNT ) < band;
    if(inBand){
      c = mixPal(PAL_FIELD, PAL_NEUT, 60); // brighter, greener
      c.w = clampu(c.w + 20);
    }
    // occasional stadium flash
    if((rnd8()&63)==0){ c.r=clampu(c.r+40); c.g=clampu(c.g+40); c.w=clampu(c.w+60); }
    flicker(c,0x0B); setPix(i,c);
  }
  if(rnd8()&1) sweepPos = (uint8_t)(sweepPos + 1);
}

// SunsetFade: smooth AMBER<->MAGENTA gradient across strip; slow drift
static void renderSunsetFade(){
  // phase advances slowly with a little randomness so it feels organic
  uint8_t step = 3 + (rnd8() & 3);   // 3..6 per frame
  uint8_t phase = sunsetPhase;

  // per-LED offset so the strip shows a spatial gradient
  // for up to 16 LEDs, i<<4 spaces the gradient nicely
  for(uint8_t i=0;i<LED_COUNT;i++){
    uint8_t t = (uint8_t)(phase + (i<<4));      // wrap automatically
    Rgbw c = mixPal(PAL_AMBER, PAL_MAGENTA, t); // warm → pink/purple
    c.w = clampu(c.w + 10);                     // a touch more backlight
    flicker(c, 0x09);
    setPix(i, c);
  }

  // occasional tiny twinkle to mimic scene cuts/reflections
  if((rnd8() & 31) == 0){
    uint8_t j = rnd8() % LED_COUNT;
    Rgbw c = mixPal(PAL_NEUT, PAL_AMBER, 220);
    c.w = clampu(c.w + 30);
    setPix(j, c);
  }

  sunsetPhase = (uint8_t)(sunsetPhase + step);
}

// ---------- Setup / Loop ----------
void setup(){
  cli(); clock_prescale_set(clock_div_1); sei();
  strip.begin(); strip.show();
#if defined(A1)
  pinMode(A1, INPUT);
  for(uint8_t i=0;i<8;i++){ rng16 ^= (uint16_t)analogRead(A1) << (i&7); }
#endif
  rng16 ^= (uint16_t)TCNT0 << 3;
  startScene();
}

static uint8_t frameCount = 0;
void loop(){
  switch(scene){
    default:
    case SC_WARMFADE:     renderWarmFade();     break;
    case SC_COOLSWEEP:    renderCoolSweep();    break;
    case SC_DARKCUTS:     renderDarkCuts();     break;
    case SC_TICKER:       renderTicker();       break;
    case SC_ADSPULSE:     renderAdsPulse();     break;
    case SC_CINEMAWIPE:   renderCinemaWipe();   break;
    case SC_AMBERGLOW:    renderAmberGlow();    break;
    case SC_MAGENTADRAMA: renderMagentaDrama(); break;
    case SC_SPORTSFIELD:  renderSportsField();  break;
    case SC_SUNSETFADE:  renderSunsetFade();  break;
  }
  strip.show();

  // More organic pacing: 16–52 ms + rare long holds
  uint8_t d = 16 + (rnd8()%37);
  if((rnd8()&31)==0) d += 70;
  delay(d);

  frameCount++;

  if(sceneMs>d) sceneMs -= d;
  else {
    if((rnd8()&3)==0 && sceneMs < 8000) sceneMs += 450 + (rnd8()*2); // extend but not too long
    else { startScene(); frameCount = 0; }
  }

  // hard fail-safe: change after ~256 frames regardless
  if(frameCount == 0){ startScene(); }
}
