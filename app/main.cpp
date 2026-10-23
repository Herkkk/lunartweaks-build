#include "polar_license.hpp"
#include "entitlements.hpp"
#include "tweaks.hpp"
#include "network_probe.hpp"
#include "telemetry.hpp"
#include "smooth_scroll.hpp"
#include "ui_audio.hpp"
#include "frame_clock.hpp"
#include <chrono>
#include <mmsystem.h>
#include <atomic>
#include <cmath>
#include <commctrl.h>
#include <dwmapi.h>
#include <functional>
#include <gdiplus.h>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <windowsx.h>
using namespace Gdiplus;
using namespace lt;
namespace {
constexpr UINT WM_WORK = WM_APP + 1;
constexpr UINT WM_SCAN_PROGRESS = WM_APP + 2;
constexpr UINT WM_FRAME = WM_APP + 3;
constexpr const wchar_t *kWebsite = L"https://lunartweaks.se";
lt::FrameClock frameClock;
ULONGLONG uiNow(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
constexpr int ID_KEY = 101;
HWND window = nullptr, keyEdit = nullptr;
ULONG_PTR gdip = 0;
std::unique_ptr<Licensing> license;
std::unique_ptr<Bitmap> brandLogo, violetLight, blueLight;
std::thread worker;
std::atomic_bool busy = false;
std::atomic_bool skipBaseline = false;
int currentOperation = 0, scanPhase = 0;
unsigned probeCount = 0;
std::wstring baselineReport, baselineSummary, lastScan;
bool reduceMotion = false, soundEnabled = true, fullScreen = false;
WINDOWPLACEMENT savedPlacement{};
LONG_PTR savedWindowStyle = 0;
std::wstring preferencesPath;
std::vector<unsigned char> clickWave, completeWave, errorWave;
ULONGLONG lastSound = 0;
void sound(lt::Cue cue) {
    if(!soundEnabled || !window || GetForegroundWindow()!=window || IsIconic(window))return;
    auto now=uiNow();
    if(cue==lt::Cue::Click && now-lastSound<90)return;
    const auto &wave=cue==lt::Cue::Click?clickWave:cue==lt::Cue::Complete?completeWave:errorWave;
    if(wave.empty())return;
    if(PlaySoundW(reinterpret_cast<LPCWSTR>(wave.data()),nullptr,SND_MEMORY|SND_ASYNC|SND_NODEFAULT|SND_NOSTOP))lastSound=now;
}
void savePreferences(){
    if(preferencesPath.empty())return;
    WritePrivateProfileStringW(L"Interface",L"Sound",soundEnabled?L"1":L"0",preferencesPath.c_str());
    WritePrivateProfileStringW(L"Interface",L"ReduceMotion",reduceMotion?L"1":L"0",preferencesPath.c_str());
}
void fitFullScreen(HWND hwnd){
    MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
    if(GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))
        SetWindowPos(hwnd,HWND_TOP,monitor.rcMonitor.left,monitor.rcMonitor.top,
                     monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,
                     SWP_FRAMECHANGED|SWP_NOACTIVATE);
}
void toggleFullScreen(){
    if(!fullScreen){
        savedPlacement.length=sizeof(savedPlacement);
        if(!GetWindowPlacement(window,&savedPlacement))return;
        savedWindowStyle=GetWindowLongPtrW(window,GWL_STYLE);
        fullScreen=true;
        SetWindowLongPtrW(window,GWL_STYLE,savedWindowStyle&~(WS_THICKFRAME|WS_MAXIMIZEBOX));
        DWORD rounding=1;DwmSetWindowAttribute(window,33,&rounding,sizeof(rounding));
        fitFullScreen(window);
    }else{
        fullScreen=false;
        SetWindowLongPtrW(window,GWL_STYLE,savedWindowStyle);
        SetWindowPlacement(window,&savedPlacement);
        SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
        DWORD rounding=2;DwmSetWindowAttribute(window,33,&rounding,sizeof(rounding));
    }
}

bool logged = false, preview = false, recovery = false, scanDone = false;
bool freeMode=false, revealKey=false, keyFocused=false, keyError=false;
std::vector<int> shownIds;
lt::SmoothScroll detailScroll;
float detailLimit=0;
bool detailFocus=false, draggingDetail=false;
float detailGrab=0;
std::wstring detailBody, detailTitle, detailPc;
int detailId=-1;
float detailTitleHeight=0, detailPcHeight=0, detailBodyHeight=0, detailContentHeight=0;
bool detailCacheDirty=true, staticRows=false;
int pressedHit=-1;
ULONGLONG hoverStart=0;
int selected = -1, scrollRow = 0, hover = -1, keyboardFocus = -1;
float scale = 1, offsetX = 0, offsetY = 0;
float tick = 0;
lt::CpuCounter cpuCounter;
lt::ActivityHistory activity;
float cpuUsage = -1, memoryUsage = -1;
int chartMode = 0;
int pageItemCount = 20;
int inspectedTweak = -1;
lt::SmoothScroll listScroll;
bool listVisible = false, listCacheDirty = true, draggingScroll = false;
int listHitStart = -1, hoveredTweak = -1;
float scrollGrab = 0;
unsigned wheelLines = 3;
int recoveryWheelDelta = 0;

// ---- Layout (logical 1280 x 800) -------------------------------------------------------
constexpr float kHeader = 56;
constexpr float kLeft = 264, kRight = 1248, kWidth = kRight - kLeft;
// Category list
constexpr float LIST_X = 264, LIST_Y = 160, LIST_W = 560, LIST_H = 600;
constexpr float ROW_X = 272, ROW_W = 532, ROW_H = 72, ROWS_Y = 172, ROWS_H = 576;
constexpr float SB_X = 812;
// Inspector
constexpr float DET_X = 840, DET_Y = 160, DET_W = 408, DET_H = 600;
constexpr float DC_X = 852, DC_Y = 180, DC_W = 384, DC_H = 484;
// Overview activity chart
constexpr float ACT_X = 264, ACT_Y = 660, ACT_W = 600, ACT_H = 100;
constexpr int kRecoveryPerPage = 8;

void updateListHits();
void prepareListCache();
void drawScrollingList(Graphics &g);
int maxScrollRow();
// <<<STATE-BEGIN>>>
ULONGLONG introStart = 0, detailSwitch = 0, navStart = 0, ringStart = 0, toastStart = 0, recentStart = 0;
float navFrom = -1, navTo = -1, ringFrom = 0, ringTo = 0;
int recentId = -1, toastKind = 0;
std::wstring toastText;
struct Modal {
    bool open = false;
    std::wstring title, body, primary, secondary;
    int tone = 0, icon = 0; // tone: 0 accent, 1 success, 2 error, 3 warning
    std::function<void()> action;
    ULONGLONG start = 0;
};
Modal modal;
// <<<STATE-END>>>
bool sampling = false, groupedCpu = false;
ULONGLONG transitionStart = 0, lastSampleTick = 0;
float hoverAmount = 0;
int animatedHover = -1;

bool buildingScene = false, sceneDirty = true, movingWindow = false, appActive = true;
ULONGLONG animationStart = 0, ambientTick=0, scrollInput=0;
float ambientPhase=0;
std::unique_ptr<PrivateFontCollection> embeddedFonts;
std::unique_ptr<FontFamily> uiFontFamily, headingFontFamily;
std::map<std::tuple<int, bool, bool>, std::unique_ptr<Font>> uiFonts;
struct Surface {
    HDC dc = nullptr;
    HBITMAP handle = nullptr, previous = nullptr;
    std::unique_ptr<Bitmap> image;
    int width = 0, height = 0;
    ~Surface() { reset(); }
    void reset() {
        image.reset();
        if (dc && previous) SelectObject(dc, previous);
        if (handle) DeleteObject(handle);
        if (dc) DeleteDC(dc);
        dc = nullptr; handle = nullptr; previous = nullptr; width = height = 0;
    }
    bool ensure(int w, int h) {
        if (dc && width == w && height == h) return false;
        reset();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *pixels = nullptr;
        dc = CreateCompatibleDC(nullptr);
        handle = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dc || !handle || !pixels) { reset(); throw std::runtime_error("Could not allocate the window surface"); }
        previous = (HBITMAP)SelectObject(dc, handle);
        image = std::make_unique<Bitmap>(w, h, w * 4, PixelFormat32bppPARGB, (BYTE *)pixels);
        width = w; height = h;
        return true;
    }
};
Surface sceneSurface, frameSurface, transitionSurface, listSurface, ambientSurface, detailSurface;
void beginTransition() {
    // Every page enters with a staggered fade-up, like the website's sections.
    animatedHover = -1;
    hoverAmount = 0;
    transitionStart = 0;
    introStart = reduceMotion ? 0 : uiNow();
}
bool sceneAnimating() {
    auto now = uiNow();
    return (introStart && now - introStart < 1400) || (navStart && now - navStart < 280) ||
           (ringStart && now - ringStart < 920) || (modal.open && now - modal.start < 240);
}
bool frameAnimating() {
    auto now = uiNow();
    return (toastStart && now - toastStart < 3900) || (detailSwitch && now - detailSwitch < 240) ||
           (recentStart && now - recentStart < 1100);
}
void sampleActivity() {
    ULONGLONG now = uiNow();
    if (lastSampleTick && now - lastSampleTick > 2500) { activity.clear(); cpuCounter.primed = false; }
    lastSampleTick = now;
    if (preview) { cpuUsage = memoryUsage = -1; return; }
    FILETIME i{}, k{}, u{};
    auto bits = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    if (GetSystemTimes(&i, &k, &u)) cpuUsage = cpuCounter.update(bits(i), bits(k), bits(u)).value_or(-1);
    else { cpuCounter.primed = false; cpuUsage = -1; }
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    memoryUsage = GlobalMemoryStatusEx(&m) ? float(m.dwMemoryLoad) : -1;
    activity.push({cpuUsage, memoryUsage, double(now) / 1000});
}
void refresh(HWND hwnd) { sceneDirty = true; InvalidateRect(hwnd, nullptr, FALSE); }
void animationTimer(HWND hwnd) {
    bool visible = !movingWindow && !IsIconic(hwnd) && appActive;
    if (!visible || reduceMotion) {listScroll.target=std::clamp(std::round(listScroll.target/ROW_H)*ROW_H,0.0f,listScroll.limit);listScroll.settle();scrollInput=0;detailScroll.settle();hoverStart=0;hoverAmount=1;}
    bool sceneMotion = !reduceMotion && sceneAnimating();
    bool motionNeeded = busy || transitionStart || listScroll.moving() || detailScroll.moving() || hoverStart || scrollInput || sceneMotion || frameAnimating();
    int interval=(!visible||reduceMotion)?((busy||frameAnimating())&&visible?50:0):sceneMotion?14:motionNeeded?8:50;
    if(frameClock.running())frameClock.interval(interval);
    else if(interval)SetTimer(hwnd,1,interval,nullptr);
    else KillTimer(hwnd,1);
    bool shouldSample = visible && logged && !preview && !recovery && selected < 0;
    if (shouldSample && !sampling) {
        activity.clear(); cpuCounter.primed = false; cpuUsage = memoryUsage = -1; lastSampleTick = 0;
        sampleActivity();
        SetTimer(hwnd, 2, 1000, nullptr);
    } else if (!shouldSample) KillTimer(hwnd, 2);
    sampling = shouldSample;
}
void invalidateRegion(HWND hwnd, RectF a) {
    RECT pixels{(LONG)std::floor(offsetX + a.X * scale), (LONG)std::floor(offsetY + a.Y * scale),
                (LONG)std::ceil(offsetX + (a.X + a.Width) * scale), (LONG)std::ceil(offsetY + (a.Y + a.Height) * scale)};
    InvalidateRect(hwnd, &pixels, FALSE);
}
void invalidateAnimation(HWND hwnd);

SystemInfo pc;
std::wstring notice = L"Choose Free or activate Pro to scan this PC.";
std::wstring stage;
std::vector<int> states(kMaxTweakId + 1, 0); // 0 manual/diagnostic, 1 ready, 2 active, 3 not available
struct Hit { RectF r; int action, index; };
std::vector<Hit> hits;
int tier() { return preview ? 2 : freeMode ? 0 : (license ? license->tier() : 0); }
int maxScrollRow() { return pageItemCount > 0 ? (pageItemCount - 1) / kRecoveryPerPage : 0; }
void updateListHits() {
    if (!listVisible || listHitStart < 0 || listHitStart + pageItemCount > (int)hits.size()) return;
    float offset = std::round(listScroll.position * scale) / scale;
    for (int i = 0; i < pageItemCount; ++i) {
        float top = ROWS_Y + i * ROW_H - offset;
        float y = std::max(ROWS_Y, top + 3), bottom = std::min(ROWS_Y + ROWS_H, top + ROW_H - 3);
        auto &hit = hits[listHitStart + i];
        hit = bottom > y ? Hit{RectF(ROW_X, y, ROW_W - 12, bottom - y), 22, shownIds[i]}
                         : Hit{RectF(-1, -1, 0, 0), -1, shownIds[i]};
    }
}
void invalidateList(HWND hwnd) { invalidateRegion(hwnd, RectF(LIST_X, LIST_Y, LIST_W, LIST_H)); }
void moveList(float target, bool immediate = false) {
    if (!listVisible || busy || !logged || recovery || selected < 0) return;
    if(reduceMotion)target=std::round(target/ROW_H)*ROW_H;
    if (!listScroll.to(target, uiNow(), immediate || reduceMotion)) return;
    transitionStart = 0;
    bool clearFocus = keyboardFocus >= 0 || (hover >= 0 && hover < (int)hits.size() && hits[hover].action != 22);
    scrollInput=immediate||reduceMotion?0:uiNow();
    keyboardFocus = hover = animatedHover = -1;
    hoveredTweak = -1;
    hoverAmount = 0;
    updateListHits();
    if (clearFocus) refresh(window); else invalidateList(window);
    animationTimer(window);
}
float scrollThumbHeight() { return std::max(40.0f, (ROWS_H - 16) * ROWS_H / std::max(ROWS_H, pageItemCount * ROW_H)); }
float scrollThumbY() { return listScroll.limit > 0 ? ROWS_Y + 8 + (ROWS_H - 16 - scrollThumbHeight()) * listScroll.position / listScroll.limit : ROWS_Y + 8; }
struct Result {
    bool success = false;
    int op = 0;
    int tweakId = 0;
    std::wstring message, baselineReport, baselineSummary;
    SystemInfo info;
    std::vector<int> states;
    bool needsAdmin = false;
};

// ---- Palette (matches lunartweaks.se) -------------------------------------------------
Color bgColor(255,7,6,13);
Color card(255,17,15,27), cardHover(255,23,20,37), cardBorder(255,38,33,57), borderHover(255,74,56,112);
Color tile(255,30,26,47), elevated(255,32,28,48), divider(255,35,31,52);
Color white(255,244,242,250), muted(255,166,160,187), dim(255,118,112,140);
Color purple(255,192,160,255), lavender(255,183,148,246), accent(255,124,58,237), accent2(255,159,103,255);
Color green(255,122,214,160), amber(255,240,190,110), red(255,236,124,136);
float gAlpha = 1;
Color fade(Color c){return gAlpha>=.999f?c:Color(BYTE(c.GetA()*std::clamp(gAlpha,0.0f,1.0f)),BYTE(c.GetR()),BYTE(c.GetG()),BYTE(c.GetB()));}
Color mix(Color a,Color b,float t){t=std::clamp(t,0.0f,1.0f);return Color(255,BYTE(a.GetR()+(b.GetR()-a.GetR())*t),BYTE(a.GetG()+(b.GetG()-a.GetG())*t),BYTE(a.GetB()+(b.GetB()-a.GetB())*t));}

// <<<PRIM-BEGIN>>>  Drawing primitives (replaced by a recorder in the portable preview harness)
void roundedPath(GraphicsPath &p,float x,float y,float w,float h,float radius){
    radius=std::max(0.5f,std::min(radius,std::min(w,h)/2));float d=radius*2;
    p.AddArc(x,y,d,d,180,90);p.AddArc(x+w-d,y,d,d,270,90);p.AddArc(x+w-d,y+h-d,d,d,0,90);p.AddArc(x,y+h-d,d,d,90,90);p.CloseFigure();
}
void rect(Graphics &g, float x, float y, float w, float h, Color c, float radius = 10) {
    if(w<=0||h<=0)return;
    GraphicsPath p;roundedPath(p,x,y,w,h,radius);SolidBrush b(fade(c));g.FillPath(&b,&p);
}
void gradientRect(Graphics &g,float x,float y,float w,float h,Color a,Color b,float radius=10){
    if(w<=0||h<=0)return;
    GraphicsPath p;roundedPath(p,x,y,w,h,radius);LinearGradientBrush brush(PointF(x-1,y),PointF(x+w+1,y),fade(a),fade(b));g.FillPath(&brush,&p);
}
void outlineRect(Graphics &g,float x,float y,float w,float h,Color color,float weight=1,float radius=10){
    GraphicsPath p;roundedPath(p,x,y,w,h,radius);Pen pen(fade(color),weight);g.DrawPath(&pen,&p);
}
void line(Graphics &g, float x, float y, float x2, float y2, Color c, float weight = 1) {
    Pen p(fade(c), weight);g.DrawLine(&p, x, y, x2, y2);
}
void glow(Graphics &g,float cx,float cy,float w,float h,Color c){
    if(gAlpha<.999f)c=fade(c);
    GraphicsPath p;p.AddEllipse(cx-w/2,cy-h/2,w,h);PathGradientBrush b(&p);b.SetCenterColor(c);
    Color edge(0,c.GetR(),c.GetG(),c.GetB());int n=1;b.SetSurroundColors(&edge,&n);g.FillPath(&b,&p);
}
Font *fontFor(float size,bool bold,bool heading){
    heading=heading&&headingFontFamily;
    auto key=std::make_tuple((int)std::lround(size*10),bold,heading);
    auto &font=uiFonts[key];
    if(!font)font=std::make_unique<Font>(heading?headingFontFamily.get():uiFontFamily.get(),size,
                                        heading?FontStyleRegular:bold?FontStyleBold:FontStyleRegular,UnitPixel);
    return font.get();
}
// flags: 1 centre, 2 right, 4 single line, 8 display font, 16 vertically centred
void text(Graphics &g, const std::wstring &value, float x, float y, float width, float height,
          float size, Color c, bool bold = false, int flags = 0) {
    if(value.empty())return;
    Font *font=fontFor(size,bold,(flags&8)||size>=20);
    SolidBrush b(fade(c));
    StringFormat fmt;
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags((flags&4)?StringFormatFlagsNoWrap:StringFormatFlagsNoClip);
    if(flags&1)fmt.SetAlignment(StringAlignmentCenter);else if(flags&2)fmt.SetAlignment(StringAlignmentFar);
    if(flags&16)fmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(value.c_str(), -1, font, RectF(x, y, width, height), &fmt, &b);
}
float textWidth(Graphics &g,const std::wstring &value,float size,bool bold=false,bool heading=false){
    StringFormat fmt;fmt.SetFormatFlags(StringFormatFlagsNoWrap);RectF bounds;
    g.MeasureString(value.c_str(),-1,fontFor(size,bold,heading||size>=20),RectF(0,0,4000,200),&fmt,&bounds);return bounds.Width;
}
float textHeight(Graphics &g,const std::wstring &value,float size,float width,bool bold=false,bool heading=false){
    if(value.empty())return 0;
    StringFormat fmt;RectF bounds;
    g.MeasureString(value.c_str(),-1,fontFor(size,bold,heading||size>=20),RectF(0,0,width,10000),&fmt,&bounds);return std::ceil(bounds.Height);
}
// <<<PRIM-END>>>

// <<<UI-BEGIN>>>
// ---- Motion: website-style staggered fade-up, eased hovers, count-ups --------------------
float progress(ULONGLONG start,float ms){if(!start||reduceMotion)return 1;return std::clamp(float(uiNow()-start)/ms,0.0f,1.0f);}
float appear(int i){
    if(!introStart||reduceMotion)return 1;
    float t=(float(uiNow()-introStart)-i*55.0f)/460.0f;
    return lt::easeOut(std::clamp(t,0.0f,1.0f));
}
struct Reveal{
    Graphics &g;int state;float saved;
    Reveal(Graphics &gr,int i,float dy=18):g(gr){float p=appear(i);saved=gAlpha;gAlpha*=p;state=(int)g.Save();g.TranslateTransform(0,(1-p)*dy);}
    ~Reveal(){g.Restore(state);gAlpha=saved;}
};
float hoverEase(int me){return hover==me?(reduceMotion?1:lt::easeOut(hoverAmount)):0;}
float ringShown(){return ringFrom+(ringTo-ringFrom)*lt::easeOut(progress(ringStart,900));}
int stateOf(int id){return id>=0&&id<(int)states.size()?states[id]:0;}
bool visibleTweak(const Tweak &t){return !t.legacy;}
std::wstring tierName(int v) { return v == 2 ? L"Pro" : L"Free"; }
struct Counts{int ready=0,active=0,unavailable=0,locked=0,lockedMatched=0,total=0,recommended=0,all=0;};
Counts countFor(int category){
    Counts c;
    for(const auto &t:catalog()){
        if(!visibleTweak(t)||(category>=0&&t.category!=category))continue;
        ++c.all;
        if(!automatic(t))continue;
        int s=stateOf(t.id);
        if(t.tier>tier()){++c.locked;if(s==1||s==2)++c.lockedMatched;continue;}
        if(s==1){++c.ready;++c.total;if(t.recommended&&!(t.id==41&&tier()==2))++c.recommended;}
        else if(s==2){++c.active;++c.total;}
        else if(s==3)++c.unavailable;
    }
    return c;
}
int proTotal(){int n=0;for(const auto&t:catalog())if(visibleTweak(t)&&t.tier==2)++n;return n;}
int freeTotal(){int n=0;for(const auto&t:catalog())if(visibleTweak(t)&&t.tier==0)++n;return n;}
const wchar_t *categoryDescriptions[categoryCount] = {
    L"A cleaner, quieter Windows: fewer animations, ads, background apps and telemetry.",
    L"Scheduler, timer and input settings that make your PC react faster.",
    L"Power plans and processor policies built for your exact CPU and power source.",
    L"GPU scheduling, interrupts and game priority matched to your graphics card.",
    L"Memory and drive settings matched to your installed RAM and drive type.",
    L"Lower latency on your active adapter and less background network traffic."};
const wchar_t *categoryShort[categoryCount] = {
    L"Fewer animations, ads and background apps.", L"Scheduler and timer tuned for fast response.",
    L"A power plan built for your exact CPU.", L"GPU scheduling, interrupts and game priority.",
    L"Matched to your RAM size and drive type.", L"Faster packets on your active adapter."};

// ---- Icons (24 x 24 grid) ----
enum Icon { IcWindows, IcBolt, IcCpu, IcGamepad, IcDrive, IcWifi, IcGrid = 10, IcUndo, IcShield, IcLock, IcCheck,
            IcSoundOn, IcSoundOff, IcMotionOn, IcMotionOff, IcFull, IcExitFull, IcSpark, IcClock, IcKey, IcArrow };
void icon(Graphics &g,int which,float x,float y,Color c,float size=24){
    auto state=g.Save();g.TranslateTransform(x,y);g.ScaleTransform(size/24,size/24);
    Color fc=fade(c);
    Pen p(fc,1.8f);p.SetStartCap(LineCapRound);p.SetEndCap(LineCapRound);p.SetLineJoin(LineJoinRound);
    SolidBrush fill(fc);
    auto lines=[&](std::initializer_list<PointF> pts){std::vector<PointF> v(pts);g.DrawLines(&p,v.data(),(int)v.size());};
    switch(which){
    case IcWindows:for(float a:{3.0f,13.0f})for(float b:{3.0f,13.0f})outlineRect(g,a,b,8,8,c,1.8f,1.5f);break;
    case IcBolt:lines({{13.5f,2},{5,13.5f},{11.5f,13.5f},{10.5f,22},{19,10.5f},{12.5f,10.5f},{13.5f,2}});break;
    case IcCpu:
        outlineRect(g,5.5f,5.5f,13,13,c,1.8f,2);outlineRect(g,9.5f,9.5f,5,5,c,1.6f,1);
        for(float i:{9.0f,12.0f,15.0f}){line(g,i,2,i,5.5f,c,1.8f);line(g,i,18.5f,i,22,c,1.8f);line(g,2,i,5.5f,i,c,1.8f);line(g,18.5f,i,22,i,c,1.8f);}
        break;
    case IcGamepad:
        outlineRect(g,2,7,20,12,c,1.8f,5.5f);line(g,7.5f,10.5f,7.5f,15.5f,c,1.8f);line(g,5,13,10,13,c,1.8f);
        g.FillEllipse(&fill,14.6f,10.4f,2.6f,2.6f);g.FillEllipse(&fill,17.2f,13.2f,2.6f,2.6f);break;
    case IcDrive:
        outlineRect(g,3,4,18,7,c,1.8f,2);outlineRect(g,3,13,18,7,c,1.8f,2);
        g.FillEllipse(&fill,15.5f,6.5f,2.0f,2.0f);g.FillEllipse(&fill,15.5f,15.5f,2.0f,2.0f);break;
    case IcWifi:
        for(float r:{13.0f,8.5f,4.0f})g.DrawArc(&p,12-r,19.5f-r,r*2,r*2,-135.0f,90.0f);
        g.FillEllipse(&fill,10.8f,18.3f,2.4f,2.4f);break;
    case IcGrid:for(float a:{3.0f,13.5f})for(float b:{3.0f,13.5f})outlineRect(g,a,b,7.5f,7.5f,c,1.8f,2);break;
    case IcUndo:g.DrawArc(&p,4.0f,4.5f,16.0f,16.0f,-160.0f,290.0f);lines({{3.6f,4.8f},{4.5f,9.7f},{9.2f,8.4f}});break;
    case IcShield:lines({{12,2.5f},{19.5f,5.5f},{19,12},{12,21},{5,12},{4.5f,5.5f},{12,2.5f}});lines({{8.5f,11.5f},{11,14},{15.5f,9}});break;
    case IcLock:outlineRect(g,5,10.5f,14,10.5f,c,1.8f,2.5f);g.DrawArc(&p,8.0f,4.0f,8.0f,11.0f,180.0f,180.0f);g.FillEllipse(&fill,10.9f,14.4f,2.2f,2.2f);break;
    case IcCheck:lines({{5,12.5f},{10,17},{19,7}});break;
    case IcSoundOn:case IcSoundOff:
        lines({{3,9.5f},{7,9.5f},{12,5},{12,19},{7,14.5f},{3,14.5f},{3,9.5f}});
        if(which==IcSoundOn){g.DrawArc(&p,10.0f,8.0f,8.0f,8.0f,-50.0f,100.0f);g.DrawArc(&p,9.0f,4.5f,13.0f,15.0f,-50.0f,100.0f);}
        else{line(g,15.5f,9.5f,20.5f,14.5f,c,1.8f);line(g,20.5f,9.5f,15.5f,14.5f,c,1.8f);}
        break;
    case IcMotionOn:g.DrawEllipse(&p,11.0f,7.0f,10.0f,10.0f);line(g,2,9,8,9,c,1.8f);line(g,4,12,8,12,c,1.8f);line(g,2,15,8,15,c,1.8f);break;
    case IcMotionOff:g.DrawEllipse(&p,7.0f,7.0f,10.0f,10.0f);line(g,4,20,20,4,c,1.8f);break;
    case IcFull:lines({{3,9},{3,3},{9,3}});lines({{15,3},{21,3},{21,9}});lines({{21,15},{21,21},{15,21}});lines({{9,21},{3,21},{3,15}});break;
    case IcExitFull:lines({{9,3},{9,9},{3,9}});lines({{15,3},{15,9},{21,9}});lines({{21,15},{15,15},{15,21}});lines({{3,15},{9,15},{9,21}});break;
    case IcSpark:lines({{12,2},{14,10},{22,12},{14,14},{12,22},{10,14},{2,12},{10,10},{12,2}});break;
    case IcClock:g.DrawEllipse(&p,3.0f,3.0f,18.0f,18.0f);lines({{12,7},{12,12},{15.5f,14}});break;
    case IcKey:g.DrawEllipse(&p,2.5f,8.0f,8.0f,8.0f);lines({{10.5f,12},{21,12}});lines({{17,12},{17,16}});lines({{20,12},{20,15}});break;
    case IcArrow:lines({{5,12},{19,12}});lines({{13,6},{19,12},{13,18}});break;
    }
    g.Restore(state);
}
void card2(Graphics &g,float x,float y,float w,float h,float hot=0,float radius=16){
    if(hot>.01f)glow(g,x+w/2,y+h/2+8,w*1.12f,h*1.7f,Color(BYTE(54*hot),124,58,237));
    rect(g,x,y,w,h,mix(card,cardHover,hot),radius);
    outlineRect(g,x+.5f,y+.5f,w-1,h-1,mix(cardBorder,borderHover,hot),1,radius);
}
void iconTile(Graphics &g,int which,float x,float y,float s=40,Color c=lavender){
    rect(g,x,y,s,s,tile,s*.26f);
    icon(g,which,x+(s-s*.5f)/2,y+(s-s*.5f)/2,c,s*.5f);
}
float lift(int hitIndex){
    bool over=hover==hitIndex;
    return over&&!reduceMotion&&pressedHit!=hitIndex?2*lt::easeOut(hoverAmount):0;
}
// Small rounded label. style: 0 neutral, 1 accent, 2 success, 3 pro, 4 dim, 5 warning
float chip(Graphics &g,const std::wstring &label,float x,float y,int style,float h=22,float minWidth=0,int ic=-1){
    float w=std::max(minWidth,textWidth(g,label,11.5f,true)+(ic>=0?34:20));
    Color fg=style==1?purple:style==2?green:style==3?white:style==4?dim:style==5?amber:muted;
    if(style==3)gradientRect(g,x,y,w,h,accent,accent2,h/2);
    else{
        Color bg=style==1?Color(255,38,26,66):style==2?Color(255,20,44,36):style==5?Color(255,48,38,22):tile;
        rect(g,x,y,w,h,bg,h/2);
    }
    float tx=x;
    if(ic>=0){icon(g,ic,x+9,y+(h-12)/2,fg,12);tx+=14;}
    text(g,label,tx,y,w-(tx-x),h,11.5f,fg,true,1|4|16);
    return w;
}
void quietButton(Graphics &g, const std::wstring &label, float x, float y, float w, float h, int action, int index=-1, bool alignLeft=false) {
    bool over = (int)hits.size() == hover;
    if (over) rect(g, x, y, w, h, elevated, 10);
    text(g, label, x + (alignLeft?10:0), y, w-(alignLeft?10:0), h, 13.5f, over ? white : muted, false, (alignLeft?0:1)|4|16);
    hits.push_back({RectF(x, y, w, h), action, index});
}
void iconButton(Graphics &g,int which,float x,float y,int action,bool active=false){
    bool over=(int)hits.size()==hover;
    if(over||active)rect(g,x,y,36,36,over?elevated:tile,10);
    icon(g,which,x+9,y+9,over?white:active?purple:muted,18);
    hits.push_back({RectF(x,y,36,36),action,-1});
}
// style: 0 secondary, 1 primary gradient, 2 outline accent
void button(Graphics &g, const std::wstring &title, float x, float y, float w, float h, int action,
            int index = -1, int style = 0, bool disabled = false, int ic = -1) {
    int me=(int)hits.size();
    bool over = me == hover, pressed=pressedHit==me;
    float amount=over&&!disabled?(reduceMotion?1:lt::easeOut(hoverAmount)):0;
    float up=reduceMotion||pressed||disabled?0:2*amount;
    float radius=h/2;
    if(disabled){rect(g,x,y,w,h,tile,radius);}
    else if(style==1){
        if(!reduceMotion)glow(g,x+w/2,y+h/2+4-up,w*1.15f,h*2.2f,Color(BYTE(40+50*amount),124,58,237));
        Color a=mix(accent,Color(255,140,80,245),amount),b=mix(accent2,Color(255,178,132,255),amount);
        if(pressed){a=mix(a,bgColor,.18f);b=mix(b,bgColor,.18f);}
        gradientRect(g,x,y-up,w,h,a,b,radius);
    }else{
        Color bg=mix(style==2?Color(255,30,22,52):elevated,Color(255,42,35,64),amount);
        if(pressed)bg=mix(bg,bgColor,.2f);
        rect(g,x,y-up,w,h,bg,radius);
        outlineRect(g,x+.5f,y-up+.5f,w-1,h-1,style==2?Color(255,110,76,190):over?borderHover:cardBorder,1,radius);
    }
    Color fg=disabled?dim:style==1?white:style==2?purple:white;
    float tw=textWidth(g,title,14,true),iw=ic>=0?22:0;
    float start=x+(w-tw-iw)/2;
    if(ic>=0)icon(g,ic,start,y-up+(h-16)/2,fg,16);
    text(g,title,start+iw,y-up,tw+8,h,14,fg,true,4|16);
    hits.push_back({disabled?RectF(-1,-1,0,0):RectF(x,y,w,h),disabled?-1:action,index});
}
void sectionLabel(Graphics &g,const std::wstring &label,float x,float y,Color c=lavender){
    text(g,label,x,y,400,18,11,c,true,4);
}
void paintBackground(Graphics &g) {
    g.Clear(bgColor);
    // Slowly drifting light, like the website hero.
    float dx=std::sin(ambientPhase*.31f)*64,dy=std::cos(ambientPhase*.23f)*42,ex=std::cos(ambientPhase*.19f)*52;
    if(violetLight)g.DrawImage(violetLight.get(),RectF(160+dx,-560+dy,1120,1120));
    if(violetLight)g.DrawImage(violetLight.get(),RectF(420-ex,380+dy*.6f,760,760));
    if(blueLight)g.DrawImage(blueLight.get(),RectF(860+ex,420-dy,700,700));
}
void brandMark(Graphics &g, float x, float y, float size) {
    if (brandLogo) g.DrawImage(brandLogo.get(), RectF(x, y, size, size));
}
void captionControl(Graphics &g, float x, int action) {
    bool over = hover == (int)hits.size();
    if (over) rect(g, x, 0, 46, kHeader, action == 99 ? Color(255, 196, 43, 60) : elevated, 1);
    Pen glyph(over ? white : Color(255, 184, 178, 204), 1.4f);
    float cx = x + 23, cy = kHeader/2;
    if (action == 98) g.DrawLine(&glyph, cx - 5, cy, cx + 5, cy);
    else { g.DrawLine(&glyph, cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f); g.DrawLine(&glyph, cx - 4.5f, cy + 4.5f, cx + 4.5f, cy - 4.5f); }
    hits.push_back({RectF(x, 0, 46, kHeader), action, -1});
}
void header(Graphics &g) {
    SolidBrush bar(Color(200,9,8,16));g.FillRectangle(&bar,0.0f,0.0f,1280.0f,kHeader);
    brandMark(g,20,12,32);
    text(g,L"Lunar",60,0,60,kHeader,17,white,true,4|8|16);
    text(g,L"Tweaks",112,0,80,kHeader,17,lavender,true,4|8|16);
    if(logged&&!recovery){
        if(preview)chip(g,L"PREVIEW",192,17,4);
        else if(tier()==2)chip(g,L"PRO",192,17,3,22,0,IcSpark);
        else chip(g,L"FREE",192,17,0);
    }
    float x=1000;
    if(logged&&!preview&&!pc.elevated&&scanDone){
        bool over=(int)hits.size()==hover;
        float w=136;
        rect(g,x-w-12,12,w,32,over?Color(255,56,40,22):Color(255,40,30,18),16);
        outlineRect(g,x-w-12+.5f,12.5f,w-1,31,Color(255,120,90,40),1,16);
        icon(g,IcShield,x-w,20,amber,16);
        text(g,L"Run as admin",x-w+20,12,w-30,32,13,amber,true,4|16);
        hits.push_back({RectF(x-w-12,12,w,32),40,-1});
    }
    iconButton(g,soundEnabled?IcSoundOn:IcSoundOff,x,10,34);
    iconButton(g,reduceMotion?IcMotionOff:IcMotionOn,x+42,10,16);
    iconButton(g,fullScreen?IcExitFull:IcFull,x+84,10,35);
    captionControl(g,1188,98);captionControl(g,1234,99);
    line(g,0,kHeader,1280,kHeader,divider);
}
void featureCard(Graphics &g,int ic,const std::wstring &title,const std::wstring &body,float x,float y,float w,float h){
    card2(g,x,y,w,h);
    iconTile(g,ic,x+20,y+(h-44)/2,44);
    text(g,title,x+80,y+16,w-100,24,16,white,true,4|8);
    text(g,body,x+80,y+40,w-100,h-44,13.5f,muted);
}
void planTile(Graphics &g,bool pro,float x,float y,float w,float h){
    if(pro){
        glow(g,x+w/2,y+h/2,w*1.4f,h*1.3f,Color(60,124,58,237));
        rect(g,x,y,w,h,Color(255,24,18,42),16);
        outlineRect(g,x+.5f,y+.5f,w-1,h-1,Color(255,124,82,224),1.2f,16);
        chip(g,L"RECOMMENDED",x+w-118,y+14,3,20,104);
    }else card2(g,x,y,w,h);
    text(g,pro?L"Pro":L"Free",x+18,y+14,120,26,18,pro?purple:white,true,8);
    text(g,std::to_wstring(pro?proTotal()+freeTotal():freeTotal())+L" tweaks",x+18,y+42,w-36,34,26,white,true,8);
    const wchar_t *freeLines[]={L"Windows essentials",L"Privacy & clean-up",L"One-click undo"};
    std::wstring proLines[]={std::to_wstring(proTotal())+L" performance boosts",L"Values matched to your PC",L"One-click Lunar Boost"};
    for(int i=0;i<3;i++){
        icon(g,IcCheck,x+18,y+88+i*24,pro?purple:green,14);
        text(g,pro?proLines[i]:std::wstring(freeLines[i]),x+40,y+84+i*24,w-52,22,13,pro?white:muted,false,4);
    }
}
void loginPage(Graphics &g){
    glow(g,360,300,900,700,Color(70,96,40,200));
    {Reveal r(g,0);
    brandMark(g,72,92,52);
    text(g,L"Lunar",136,92,90,52,24,white,true,4|16);
    text(g,L"Tweaks",214,92,140,52,24,lavender,true,4|16);}
    {Reveal r(g,1,24);text(g,L"Performance,",72,176,580,60,48,white,true);}
    {Reveal r(g,2,24);text(g,L"made personal.",72,232,580,60,48,lavender,true);}
    {Reveal r(g,3);text(g,L"LunarTweaks reads your CPU, GPU, memory and drive, then tunes Windows for that exact PC. Every change is saved first and can be undone.",
         72,306,530,52,15.5f,muted);}
    {Reveal r(g,4);featureCard(g,IcCpu,L"Hardware-aware tuning",L"Values chosen for your CPU, GPU and RAM, not copied from a forum post.",72,392,540,84);}
    {Reveal r(g,5);featureCard(g,IcGamepad,L"Built for games",L"Scheduler, GPU, timer and network tweaks that cut stutter and latency.",72,488,540,84);}
    {Reveal r(g,6);featureCard(g,IcUndo,L"One-click restore",L"Every change is logged. Undo one tweak or roll everything back.",72,584,540,84);}
    {Reveal r(g,7);text(g,L"No account needed for Free · Pro keys are bound to this PC",72,700,540,22,12.5f,dim);}

    float px=664,py=80,pw=548,ph=672;
    Reveal panel(g,2,26);
    card2(g,px,py,pw,ph,false,22);
    text(g,L"Choose your plan",px+32,py+28,400,36,26,white,true);
    text(g,L"Start free, or unlock every performance tweak with Pro.",px+32,py+66,480,22,14,muted);
    planTile(g,false,px+32,py+104,232,170);
    planTile(g,true,px+284,py+104,232,170);
    text(g,L"Pro license key",px+32,py+300,300,20,13.5f,white,true);
    rect(g,px+32,py+326,484,52,Color(255,10,8,18),14);
    outlineRect(g,px+32.5f,py+326.5f,483,51,keyError?red:keyFocused?purple:Color(255,58,49,82),keyFocused?1.6f:1,14);
    icon(g,IcKey,px+46,py+342,dim,20);
    hits.push_back({RectF(px+72,py+334,352,36),33,-1});
    quietButton(g,revealKey?L"Hide":L"Show",px+436,py+336,70,32,31);
    text(g,notice,px+32,py+386,484,36,13,keyError?red:muted);
    button(g,L"Activate Pro & scan",px+32,py+428,484,50,1,-1,1,false,IcSpark);
    button(g,L"Continue with Free",px+32,py+490,484,50,30,-1,0,false,IcArrow);
    line(g,px+32,py+566,px+516,py+566,divider);
    {
        bool over=(int)hits.size()==hover;
        text(g,L"Get a Pro key at lunartweaks.se",px+32,py+580,280,32,13.5f,over?white:purple,true,4|16);
        hits.push_back({RectF(px+32,py+580,250,32),41,-1});
    }
    quietButton(g,L"Local recovery",px+300,py+580,118,32,12);
    quietButton(g,L"Preview",px+424,py+580,92,32,2);
    text(g,license&&license->polar()?L"Bought Pro? Your key is in the Polar receipt email. Free works offline.":L"Free works offline. Pro verifies your key once, then binds it to this PC.",px+32,py+626,484,22,12,dim);
}

void sidebar(Graphics &g) {
    constexpr float SW = 244, NW = 220;
    SolidBrush side(Color(225,10,9,18));
    g.FillRectangle(&side, 0.0f, kHeader+1, SW, 800-kHeader-1);
    line(g,SW,kHeader+1,SW,800,divider);
    sectionLabel(g,L"WORKSPACE",24,74,dim);
    {   // The selection pill glides between items, like the website's tab indicator.
        float target=recovery?424.0f:selected<0?96.0f:140.0f+selected*44;
        if(navTo!=target){float from=navTo<0?target:navFrom+(navTo-navFrom)*lt::easeOut(progress(navStart,260));navFrom=from;navTo=target;navStart=uiNow();}
        float y=navFrom+(navTo-navFrom)*lt::easeOut(progress(navStart,260));
        glow(g,12+NW/2,y+20,NW*1.1f,90,Color(40,124,58,237));
        rect(g, 12, y, NW, 40, Color(255,36,26,62), 12);
        outlineRect(g,12.5f,y+.5f,NW-1,39,Color(255,82,56,134),1,12);
        rect(g,12,y+11,3,18,lavender,1.5f);
    }
    auto nav = [&](const std::wstring &title, int id, float y, int ic, int action) {
        bool active = action==6 ? recovery : (!recovery && selected == id);
        bool over = (int)hits.size() == hover;
        if (over && !active) rect(g, 12, y, NW, 40, Color(BYTE(255*hoverEase((int)hits.size())),22,19,35), 12);
        icon(g,ic,26,y+11,active?purple:over?white:muted,18);
        float textEnd=12+NW-10;
        if(id>=0&&action==3&&scanDone&&!preview){
            auto c=countFor(id);
            std::wstring label;int style=0;
            if(tier()<2&&c.lockedMatched>0){label=L"+"+std::to_wstring(c.lockedMatched);style=1;}
            else if(c.ready>0)label=std::to_wstring(c.ready);
            if(!label.empty()){
                float cw=std::max(26.0f,textWidth(g,label,11.5f,true)+16);
                chip(g,label,12+NW-10-cw,y+10,style,20,cw,-1);
                textEnd-=cw+6;
            }
        }
        text(g, title, 56, y, textEnd-56, 40, 14, active||over ? white : muted, true, 4|16);
        hits.push_back({RectF(12, y, NW, 40), action, id});
    };
    nav(L"Overview", -1, 96, IcGrid, 3);
    for (int i = 0; i < categoryCount; ++i) nav(categories[i], i, 140.0f + i * 44, i, 3);
    line(g,24,412,SW-24,412,divider);
    nav(L"Undo & backups",-1,424,IcUndo,6);

    float y=618;
    if(preview){
        card2(g,12,y,220,142,false,14);
        text(g,L"Design preview",28,y+16,180,22,15,white,true,8);
        text(g,L"Sample hardware, nothing is changed.",28,y+40,188,40,12.5f,muted);
        button(g,L"Exit preview",28,y+90,188,36,7);
    }else if(recovery&&!logged){
        card2(g,12,y,220,142,false,14);
        text(g,L"Local recovery",28,y+16,180,22,15,white,true,8);
        text(g,L"Restore saved values without a license.",28,y+40,188,40,12.5f,muted);
        button(g,L"Back to sign in",28,y+90,188,36,7);
    }else if(tier()<2){
        glow(g,122,y+70,270,200,Color(55,124,58,237));
        rect(g,12,y,220,142,Color(255,22,17,38),14);
        outlineRect(g,12.5f,y+.5f,219,141,Color(255,96,66,170),1,14);
        text(g,L"Free plan",28,y+14,120,22,15,white,true,8);
        chip(g,std::to_wstring(freeTotal())+L" tweaks",150,y+14,0,22,70);
        int matched=scanDone?countFor(-1).lockedMatched:proTotal();
        text(g,std::to_wstring(matched)+L" Pro boosts "+(scanDone?L"matched to this PC":L"available"),28,y+40,192,36,12.5f,purple);
        button(g,L"Upgrade to Pro",28,y+80,188,36,41,-1,1,false,IcSpark);
        quietButton(g,L"I have a key",28,y+118,188,22,32);
    }else{
        card2(g,12,y,220,142,false,14);
        text(g,L"Pro plan",28,y+16,120,22,15,white,true,8);
        chip(g,L"ACTIVE",160,y+16,2);
        text(g,L"Bound to this PC · "+std::to_wstring(proTotal()+freeTotal())+L" tweaks unlocked",28,y+42,188,40,12.5f,muted);
        button(g,L"Sign out",28,y+92,188,34,7);
    }
}
std::wstring rowSubtitle(const Tweak &t){
    bool locked=tier()<t.tier;
    if(locked&&scanDone){auto it=pc.headline.find(t.id);if(it!=pc.headline.end()&&!it->second.empty()&&stateOf(t.id)!=3)return it->second;}
    if(!locked&&scanDone&&tier()==2&&t.tier==2){auto it=pc.headline.find(t.id);if(it!=pc.headline.end()&&!it->second.empty()&&stateOf(t.id)!=3)return it->second;}
    return t.summary;
}
void statusPill(Graphics &g,const Tweak &t,float x,float y){
    bool locked=tier()<t.tier;int s=stateOf(t.id);
    if(locked){chip(g,L"PRO",x,y,3,26,84,IcLock);return;}
    if(!automatic(t)){chip(g,t.kind==Kind::Ping?L"Test":L"Run",x,y,0,26,84);return;}
    if(preview){chip(g,L"Preview",x,y,4,26,84);return;}
    if(!scanDone){chip(g,L"Scan",x,y,4,26,84);return;}
    if(s==2){chip(g,L"Active",x,y,2,26,84,IcCheck);return;}
    if(s==3){chip(g,L"N/A",x,y,4,26,84);return;}
    chip(g,L"Apply",x,y,1,26,84);
}
void controlRow(Graphics &g, const Tweak &t, float y) {
    bool active = !staticRows && inspectedTweak == t.id;
    bool over = !staticRows && hoveredTweak == t.id;
    if (active) {
        rect(g, ROW_X, y + 3, ROW_W - 12, ROW_H - 6, Color(255,32,26,52), 12);
        rect(g, ROW_X+1, y + 24, 3, 24, lavender, 1.5f);
    } else if (over) rect(g, ROW_X, y + 3, ROW_W - 12, ROW_H - 6, Color(255,24,21,38), 12);
    bool locked=tier()<t.tier;
    text(g, t.name, ROW_X+18, y + 13, 380, 24, 15, locked?Color(255,214,206,235):white, true, 4);
    text(g, rowSubtitle(t), ROW_X+18, y + 38, 392, 20, 13, locked&&scanDone?Color(255,170,146,226):muted, false, 4);
    statusPill(g,t,ROW_X+ROW_W-12-100,y+23);
    if (!staticRows && keyboardFocus >= 0 && keyboardFocus < (int)hits.size() &&
        hits[keyboardFocus].action == 22 && hits[keyboardFocus].index == t.id)
        outlineRect(g, ROW_X+1, y + 4, ROW_W - 14, ROW_H - 8, purple, 2, 12);
}
std::wstring impactLabel(int i){return i>=2?L"High impact":i==1?L"Medium impact":L"Light";}
void drawDetailContent(Graphics&g){
    const Tweak *t=findTweak(detailId);if(!t)return;
    float x=DC_X+8,w=DC_W-24;
    text(g,detailTitle,x,DC_Y,w,detailTitleHeight,22,white,true);
    float y=DC_Y+detailTitleHeight+8,cx=x;
    if(t->tier==2)cx+=chip(g,L"PRO",cx,y,3,22,0,IcSpark)+6;else cx+=chip(g,L"FREE",cx,y,0)+6;
    if(automatic(*t))cx+=chip(g,impactLabel(t->impact),cx,y,t->impact>=2?1:0)+6;
    if(t->restart)cx+=chip(g,L"Restart",cx,y,0)+6;
    auto admin=pc.admin.find(t->id);
    if(admin!=pc.admin.end()&&admin->second)chip(g,L"Admin",cx,y,pc.elevated?0:5,22,0,IcShield);
    y+=40;
    sectionLabel(g,tier()<t->tier?L"WHAT PRO DOES ON YOUR PC":L"ON YOUR PC",x,y);
    y+=22;
    rect(g,x-4,y,w+8,detailPcHeight+24,Color(255,24,20,40),12);
    text(g,detailPc,x+10,y+12,w-20,detailPcHeight+4,13,Color(255,214,208,236));
    y+=detailPcHeight+24+20;
    if(!detailBody.empty()){
        sectionLabel(g,L"WHAT IT DOES",x,y);
        text(g,detailBody,x,y+22,w,detailBodyHeight+4,13.5f,muted);
    }
}
std::wstring pcTextFor(const Tweak &t){
    if(preview)return L"Preview uses sample hardware. Activate Pro or continue with Free to see the exact values for this PC.";
    if(t.kind==Kind::Ping)return L"Sends 6 ICMP packets to Cloudflare 1.1.1.1 and shows response times. No setting is changed.";
    if(t.kind==Kind::Maintenance)return L"Clears the DNS resolver cache. No setting is changed and nothing needs to be undone.";
    if(!scanDone)return L"Scan your PC to see exactly what this changes on your hardware.";
    auto it=pc.tailoring.find(t.id);
    if(it!=pc.tailoring.end())return it->second;
    return std::wstring(t.summary)+L".";
}
void controlDetail(Graphics &g,const Tweak&t){
    bool locked=tier()<t.tier;int s=stateOf(t.id);
    card2(g,DET_X,DET_Y,DET_W,DET_H,false,18);
    std::wstring body=t.detail&&*t.detail?std::wstring(t.detail):std::wstring();
    std::wstring pcText=pcTextFor(t);
    if(detailBody!=body||detailTitle!=t.name||detailPc!=pcText||detailId!=t.id){detailBody=body;detailTitle=t.name;detailPc=pcText;detailId=t.id;detailCacheDirty=true;}
    float w=DC_W-24;
    detailTitleHeight=textHeight(g,t.name,22,w,true,true)+4;
    detailPcHeight=textHeight(g,pcText,13,w-20)+4;
    detailBodyHeight=textHeight(g,body,13.5f,w)+4;
    detailContentHeight=std::max(DC_H,detailTitleHeight+8+40+22+detailPcHeight+24+20+(body.empty()?0:22+detailBodyHeight)+12);
    detailScroll.configure(detailContentHeight,DC_H);detailLimit=detailScroll.limit;
    hits.push_back({RectF(DC_X,DC_Y,DC_W,DC_H),36,-1});
    line(g,DET_X+20,DET_Y+DET_H-92,DET_X+DET_W-20,DET_Y+DET_H-92,divider);
    auto admin=pc.admin.find(t.id);bool needsAdmin=admin!=pc.admin.end()&&admin->second;
    std::wstring status=preview?L"Preview · read only":locked?L"Pro · already matched to your hardware":!automatic(t)?L"Diagnostic · nothing to undo"
        :!scanDone?L"Scan required":s==2?L"Active · original saved in Undo":s==3?L"Not available on this PC"
        :needsAdmin&&!pc.elevated?L"Needs administrator · use Run as admin":L"Your current value is saved before applying";
    Color statusColor=s==2&&!locked&&scanDone?green:needsAdmin&&!pc.elevated&&!locked&&s==1?amber:muted;
    text(g,status,DET_X+20,DET_Y+DET_H-80,DET_W-40,20,12.5f,statusColor,false,4);
    float by=DET_Y+DET_H-52,bx=DET_X+20,bw=DET_W-40;
    if(locked)button(g,L"Unlock with Pro",bx,by,bw,38,41,t.id,1,false,IcSpark);
    else if(preview)button(g,L"Read-only preview",bx,by,bw,38,10,t.id,0,true);
    else if(t.kind==Kind::Ping)button(g,L"Run ping test",bx,by,bw,38,10,t.id,1);
    else if(t.kind==Kind::Maintenance)button(g,L"Flush DNS cache",bx,by,bw,38,10,t.id,1);
    else if(!scanDone)button(g,L"Scan this PC first",bx,by,bw,38,4,-1,1);
    else if(s==2)button(g,hasBackup(t)?L"Restore original":L"Already active",bx,by,bw,38,11,t.id,0,!hasBackup(t),IcUndo);
    else if(s==3)button(g,L"Not available on this PC",bx,by,bw,38,10,t.id,0,true);
    else button(g,L"Apply tweak",bx,by,bw,38,10,t.id,1,false,IcBolt);
}
void pageTitle(Graphics &g,const std::wstring &title,const std::wstring &sub){
    text(g,title,kLeft,68,620,42,30,white,true);
    text(g,sub,kLeft,110,620,22,14,muted,false,4);
}
void hardwareField(Graphics &g,const std::wstring &label,const std::wstring &value,float x,float y,float w){
    text(g,label,x,y,w,16,11.5f,dim,true,4);
    text(g,value.empty()?L"Not scanned":value,x,y+16,w,20,13.5f,white,false,4);
}
void drawActivity(Graphics &g) {
    float value = preview ? -1 : (chartMode == 0 ? cpuUsage : memoryUsage);
    float savedAlpha=gAlpha;gAlpha=appear(11);
    auto saved = g.Save();
    g.TranslateTransform(0,(1-gAlpha)*18);
    g.SetClip(RectF(ACT_X+2, ACT_Y+2, ACT_W-4, ACT_H-4), CombineModeIntersect);
    rect(g,ACT_X+18,ACT_Y+38,124,50,card,4);
    text(g, value < 0 ? L"—" : std::to_wstring(int(std::lround(value))) + L"%", ACT_X+20, ACT_Y+38, 120, 36, 28, white, true);
    text(g, chartMode == 0 ? L"CPU load" : L"Memory in use", ACT_X+20, ACT_Y+72, 130, 18, 12, muted, false, 4);
    const float x = ACT_X+170, y = ACT_Y+40, w = ACT_W-190, h = 46;
    rect(g,x-4,y-4,w+8,h+10,card,4);
    for (int i = 0; i < 3; ++i) line(g, x, y + i * h / 2, x + w, y + i * h / 2, Color(255,30,27,46));
    double now = double(uiNow()) / 1000;
    auto plot = preview ? lt::ActivityPlot{} : lt::plotHistory(activity, chartMode == 1, now);
    if (plot.hasSegment) {
        Pen trace(fade(lavender), 2);
        trace.SetLineJoin(LineJoinRound);
        for (size_t i = 1; i < plot.count; ++i) {
            if (!plot.points[i].connected) continue;
            const auto &a = plot.points[i - 1], &b = plot.points[i];
            g.DrawLine(&trace, x + w - float(now - a.seconds) * w / 60, y + h - a.value * h / 100,
                       x + w - float(now - b.seconds) * w / 60, y + h - b.value * h / 100);
        }
    } else text(g, preview ? L"Live data appears after activation" : L"Collecting live readings…", x, y+12, w, 22, 13, dim, false, 1|4);
    g.Restore(saved);
    gAlpha=savedAlpha;
}
void activityCard(Graphics &g){
    card2(g,ACT_X,ACT_Y,ACT_W,ACT_H);
    text(g,L"Live activity",ACT_X+20,ACT_Y+12,200,22,15,white,true,8);
    text(g,L"Last 60 seconds",ACT_X+140,ACT_Y+14,160,20,12,dim,false,4);
    float sx=ACT_X+ACT_W-136,sy=ACT_Y+10;
    rect(g,sx,sy,120,28,Color(255,10,9,18),14);
    for(int i=0;i<2;i++){
        bool on=chartMode==i,over=(int)hits.size()==hover;
        if(on||over)rect(g,sx+2+i*58,sy+2,58,24,on?Color(255,48,34,84):tile,12);
        text(g,i==0?L"CPU":L"RAM",sx+2+i*58,sy+2,58,24,12,on?purple:muted,true,1|4|16);
        hits.push_back({RectF(sx+2+i*58,sy+2,58,24),18,i});
    }
}
void overviewStatus(Graphics &g){
    float x=kLeft,y=150,w=600,h=190;
    card2(g,x,y,w,h,false,18);
    if(!scanDone||preview){
        glow(g,x+110,y+95,260,220,Color(70,124,58,237));
        iconTile(g,IcCpu,x+28,y+30,56);
        text(g,preview?L"Design preview":L"Scan your PC",x+104,y+30,460,32,24,white,true);
        text(g,preview?L"Sample hardware is shown. Activate Pro or continue with Free to read this PC.":
                      L"LunarTweaks reads your CPU, GPU, memory, drive and network, then matches every tweak to this PC. Nothing is changed by scanning.",
             x+104,y+66,460,60,14,muted);
        if(!preview)button(g,L"Scan this PC",x+104,y+130,180,40,4,-1,1,false,IcBolt);
        return;
    }
    auto c=countFor(-1);
    float cx=x+96,cy=y+95,r=58;
    glow(g,cx,cy,220,220,Color(60,124,58,237));
    Pen track(fade(Color(255,34,29,54)),10);g.DrawEllipse(&track,cx-r,cy-r,r*2,r*2);
    float target=c.total?float(c.active)/c.total:0;
    if(ringTo!=target){ringFrom=ringStart?ringShown():0;ringTo=target;ringStart=uiNow();}
    float ratio=ringShown()*appear(2);
    if(ratio>.003f){
        Pen halo(fade(Color(70,183,148,246)),18);halo.SetStartCap(LineCapRound);halo.SetEndCap(LineCapRound);
        g.DrawArc(&halo,cx-r,cy-r,r*2,r*2,-90.0f,std::max(4.0f,360*ratio));
        Pen arc(fade(lavender),10);arc.SetStartCap(LineCapRound);arc.SetEndCap(LineCapRound);g.DrawArc(&arc,cx-r,cy-r,r*2,r*2,-90.0f,std::max(4.0f,360*ratio));
    }
    auto count=[&](int v){return std::to_wstring((int)std::lround(v*appear(2)));};
    text(g,count(c.active),cx-50,cy-30,100,40,32,white,true,1);
    text(g,L"of "+std::to_wstring(c.total)+L" active",cx-50,cy+10,100,18,12,muted,false,1|4);
    float tx=x+190;
    text(g,c.ready==0?L"Your PC is fully tuned":std::to_wstring(c.ready)+L" tweaks ready for this PC",tx,y+26,390,30,21,white,true);
    text(g,L"Matched to "+(pc.cpu.empty()?std::wstring(L"your CPU"):pc.cpu),tx,y+60,390,20,13,muted,false,4);
    if(!lastScan.empty())text(g,L"Last scan "+lastScan,tx,y+80,390,20,12,dim,false,4);
    auto stat=[&](float sx,const std::wstring &label,int value,Color c2){
        rect(g,sx,y+112,124,56,tile,12);
        text(g,count(value),sx+14,y+116,100,30,22,c2,true);
        text(g,label,sx+14,y+146,104,18,11.5f,muted,true,4);
    };
    stat(tx,L"READY",c.ready,purple);
    stat(tx+134,L"ACTIVE",c.active,green);
    if(tier()<2)stat(tx+268,L"PRO MATCHED",c.lockedMatched,white);
    else stat(tx+268,L"NOT FOR THIS PC",c.unavailable,muted);
}
void hardwareCard(Graphics &g){
    float x=880,y=150,w=368,h=190;
    int me=(int)hits.size();bool over=me==hover;
    card2(g,x,y,w,h,hoverEase(me),18);
    text(g,L"Your hardware",x+20,y+16,220,24,16,white,true,8);
    text(g,L"Details",x+w-90,y+16,70,22,12.5f,over?white:purple,true,2|4);
    hits.push_back({RectF(x,y,w,h),17,-1});
    hardwareField(g,L"PROCESSOR",pc.cpu,x+20,y+50,w-40);
    hardwareField(g,L"GRAPHICS",pc.gpu,x+20,y+88,w-40);
    hardwareField(g,L"MEMORY",pc.ram?std::to_wstring(pc.ram)+L" GB":L"",x+20,y+126,150);
    hardwareField(g,L"STORAGE",pc.storage,x+190,y+126,160);
    if(pc.cores)text(g,std::to_wstring(pc.physicalCores)+L" cores · "+std::to_wstring(pc.cores)+L" threads · "+pc.network+(pc.laptop?L" · Laptop":L" · Desktop"),
                     x+20,y+164,w-40,18,11.5f,dim,false,4);
}
void areaCards(Graphics &g){
    {Reveal r(g,4);
    text(g,L"Optimization areas",kLeft,356,500,28,20,white,true);
    text(g,std::to_wstring(categoryCount)+L" areas · "+std::to_wstring(proTotal()+freeTotal())+L" tweaks",kRight-260,362,260,20,12.5f,dim,false,2|4);}
    for(int i=0;i<categoryCount;i++){
        float w=316,h=116,x=kLeft+(i%3)*(w+18),y=392+(i/3)*(h+14);
        int me=(int)hits.size();float up=lift(me)*1.5f;
        Reveal rv(g,5+i);
        auto st=g.Save();g.TranslateTransform(0,-up);
        card2(g,x,y,w,h,hoverEase(me));
        iconTile(g,i,x+18,y+18,42);
        text(g,categories[i],x+74,y+18,w-90,22,16,white,true,4|8);
        auto c=countFor(i);
        std::wstring meta=!scanDone||preview?std::to_wstring(c.all)+L" tweaks":
            std::to_wstring(c.ready)+L" ready · "+std::to_wstring(c.active)+L" active";
        text(g,meta,x+74,y+40,w-90,18,12,c.ready?purple:dim,false,4);
        text(g,categoryShort[i],x+18,y+72,w-36,40,13,muted);
        if(tier()<2&&c.locked>0)chip(g,L"+"+std::to_wstring(scanDone?c.lockedMatched:c.locked)+L" PRO",x+w-80,y+18,1,20,64);
        g.Restore(st);
        hits.push_back({RectF(x,y,w,h),3,i});
    }
}
void proCard(Graphics &g){
    float x=880,y=ACT_Y,w=368,h=ACT_H;
    if(tier()<2&&!preview){
        glow(g,x+w/2,y+h/2,w*1.2f,h*2,Color(70,124,58,237));
        rect(g,x,y,w,h,Color(255,25,18,44),18);
        outlineRect(g,x+.5f,y+.5f,w-1,h-1,Color(255,128,86,230),1.2f,18);
        auto c=countFor(-1);
        int n=scanDone?c.lockedMatched:proTotal();
        text(g,std::to_wstring(n)+L" Pro boosts "+(scanDone?L"matched to your PC":L"waiting"),x+20,y+14,w-40,22,15.5f,white,true,4|8);
        std::wstring teaser=L"Lunar Performance plan, GPU scheduling, timer resolution and more";
        if(scanDone)for(int id:{308,104,301,323,314}){auto it=pc.headline.find(id);if(it!=pc.headline.end()&&stateOf(id)==1){teaser=it->second;break;}}
        text(g,teaser,x+20,y+38,w-40,18,12.5f,purple,false,4);
        button(g,L"Unlock Pro",x+20,y+60,150,30,41,-1,1,false,IcSpark);
        quietButton(g,L"See what's included",x+178,y+60,170,30,3,3);
    }else{
        card2(g,x,y,w,h,false,18);
        iconTile(g,IcUndo,x+20,y+26,44);
        int saved=0;if(!preview)for(const auto&t:catalog())if(automatic(t)&&hasBackup(t))++saved;
        text(g,L"Undo & backups",x+80,y+22,200,22,16,white,true,4|8);
        text(g,std::to_wstring(saved)+L" original values saved on this PC",x+80,y+46,w-100,20,12.5f,muted,false,4);
        quietButton(g,L"Open",x+w-84,y+30,64,36,6);
    }
}
void workspace(Graphics &g) {
    if (selected < 0) {
        {Reveal r(g,0);pageTitle(g,L"Overview",L"Tuned for your hardware. Every change can be undone.");}
        auto c=countFor(-1);
        if(scanDone&&!preview){
            Reveal r(g,1);
            button(g,L"Scan again",kRight-352,74,140,40,4,-1,0,false,IcClock);
            button(g,c.recommended?L"Apply recommended ("+std::to_wstring(c.recommended)+L")":L"All recommended applied",kRight-200,74,200,40,13,-1,1,c.recommended==0,IcBolt);
        }
        {Reveal r(g,2);overviewStatus(g);}
        {Reveal r(g,3);hardwareCard(g);}
        areaCards(g);
        {Reveal r(g,11);activityCard(g);}
        {Reveal r(g,12);proCard(g);}
        return;
    }
    shownIds.clear();
    for(int pass=0;pass<2;pass++)
        for(const auto&t:catalog()){
            if(!visibleTweak(t)||t.category!=selected)continue;
            bool first=tier()==2?t.tier==2:t.tier==0;
            if((pass==0)==first)shownIds.push_back(t.id);
        }
    pageItemCount=(int)shownIds.size();
    {Reveal r(g,0);pageTitle(g,categories[selected],categoryDescriptions[selected]);}
    auto c=countFor(selected);
    {Reveal buttonsReveal(g,1);
    if(scanDone&&!preview){
        if(tier()<2&&c.lockedMatched>0)button(g,L"Unlock "+std::to_wstring(c.lockedMatched)+L" Pro",kRight-412,74,196,40,41,-1,2,false,IcLock);
        else if(selected==5&&!baselineReport.empty())button(g,L"Network report",kRight-372,74,156,40,15);
        button(g,c.recommended?L"Apply "+std::to_wstring(c.recommended)+L" recommended":L"All applied",kRight-200,74,200,40,42,selected,1,c.recommended==0,IcBolt);
    }else if(!preview)button(g,L"Scan this PC",kRight-180,74,180,40,4,-1,1,false,IcBolt);
    }
    listVisible = true;
    listScroll.configure(pageItemCount*ROW_H, ROWS_H);
    if (std::find(shownIds.begin(),shownIds.end(),inspectedTweak)==shownIds.end()&&!shownIds.empty())
        inspectedTweak = shownIds.front();
    {Reveal r(g,2);card2(g, LIST_X, LIST_Y, LIST_W, LIST_H, false, 18);}
    listHitStart = (int)hits.size();
    for (int i = 0; i < pageItemCount; ++i) hits.push_back({RectF(-1, -1, 0, 0), -1, -1});
    updateListHits();
    hits.push_back({RectF(SB_X-4, ROWS_Y, 16, ROWS_H), 24, -1});
    if(auto t=findTweak(inspectedTweak)){Reveal r(g,3);controlDetail(g, *t);}
}
void recoveryCard(Graphics &g,const Tweak &t,float x,float y){
    float w=484,h=112;
    card2(g,x,y,w,h,false,16);
    iconTile(g,t.legacy?IcClock:t.category,x+18,y+18,40);
    text(g,t.name,x+72,y+18,w-220,22,15,white,true,4);
    text(g,t.legacy?L"Saved by LunarTweaks 1.x":std::wstring(categories[t.category])+L" · "+tierName(t.tier),x+72,y+42,w-220,18,12.5f,muted,false,4);
    text(g,L"Exact original value saved on this PC",x+18,y+78,w-170,18,12,dim,false,4);
    button(g,L"Restore",x+w-138,y+64,118,34,11,t.id,0,preview,IcUndo);
}
void recoveryPage(Graphics &g){
    std::vector<const Tweak*>items;if(!preview)for(const auto&t:catalog())if(automatic(t)&&hasBackup(t))items.push_back(&t);
    pageItemCount=(int)items.size();scrollRow=std::clamp(scrollRow,0,maxScrollRow());
    Reveal titleReveal(g,0);
    pageTitle(g,L"Undo & backups",items.empty()?L"Nothing to restore yet. Originals are saved here automatically before every change.":
              std::to_wstring(items.size())+L" original values saved. Restoring never needs a license or internet.");
    if(items.empty()){
        card2(g,kLeft,160,kWidth,220,false,18);
        glow(g,kLeft+90,270,240,200,Color(60,124,58,237));
        iconTile(g,IcUndo,kLeft+40,200,64);
        text(g,preview?L"Recovery preview":L"Your originals will appear here",kLeft+128,198,760,36,24,white,true);
        text(g,L"Every automatic tweak saves the exact value it replaces before the first change. Restore one tweak or everything at once, any time, even without a license.",
             kLeft+128,240,780,48,14,muted);
        button(g,logged?L"Back to overview":L"Back to sign in",kLeft+128,306,200,40,logged?3:7,-1,0);
        return;
    }
    button(g,L"Restore everything",kRight-200,74,200,40,43,-1,1,preview,IcUndo);
    int first=scrollRow*kRecoveryPerPage;
    for(int i=0;i<kRecoveryPerPage&&first+i<(int)items.size();++i){
        Reveal r(g,1+i);recoveryCard(g,*items[first+i],kLeft+(i%2)*500.0f,160+(i/2)*128.0f);}
    if(pageItemCount>kRecoveryPerPage){
        text(g,L"Page "+std::to_wstring(scrollRow+1)+L" of "+std::to_wstring(maxScrollRow()+1),kLeft,720,300,24,13,muted,false,4|16);
        button(g,L"Previous",kRight-296,714,140,38,20,-1,0,scrollRow==0);
        button(g,L"Next",kRight-140,714,140,38,21,-1,0,scrollRow>=maxScrollRow());
    }
}
void busyOverlay(Graphics &g) {
    SolidBrush shade(Color(236,6,5,12));g.FillRectangle(&shade,0.0f,kHeader+1,1280.0f,800-kHeader-1);
    float x=380,y=230,w=520,h=320;
    glow(g,640,390,760,560,Color(50,124,58,237));
    card2(g,x,y,w,h,false,22);
    text(g, currentOperation == 2 ? L"Reading your PC" : currentOperation == 1 ? L"Activating Pro"
            : currentOperation == 7 ? L"Applying tweaks" : currentOperation == 4 ? L"Restoring" : L"Working",
         x+108, y+34, w-140, 32, 22, white, true);
    text(g, stage, x+108, y+68, w-140, 22, 14, muted, false, 4);
    std::wstring detail = L"This only takes a moment.";
    if (currentOperation == 2)
        detail = scanPhase == 0 ? L"Reading CPU, graphics, memory, drive and Windows details."
               : scanPhase == 1 ? L"Matching every tweak to this hardware."
                                : L"Measuring a network baseline. You can skip this step.";
    text(g, detail, x+36, y+130, w-72, 44, 14, Color(255,214,208,236));
    if (currentOperation == 2) {
        const wchar_t *steps[] = {L"Hardware", L"Matching", L"Network"};
        int n=freeMode?2:3;float sw=(w-72-(n-1)*12)/n;
        for (int i = 0; i < n; i++) {
            float sx = x+36 + i * (sw+12);
            rect(g, sx, y+190, sw, 4, i <= scanPhase ? lavender : divider, 2);
            text(g, steps[i], sx, y+202, sw, 20, 12.5f, i == scanPhase ? white : muted, i == scanPhase, 4);
        }
        if(!freeMode){
            text(g, scanPhase == 2 ? std::to_wstring(probeCount) + L" / 30 probes · not your game-server ping"
                                   : L"Scanning never changes a setting.", x+36, y+228, w-72, 20, 12.5f, dim, false, 4);
            button(g, skipBaseline ? L"Finishing…" : L"Skip network check", x+36, y+258, w-72, 38, 14, -1, 0, scanPhase < 2 || skipBaseline.load(), IcArrow);
        }else text(g,L"Local scan · no key or internet needed.",x+36,y+236,w-72,20,12.5f,dim,false,4);
    } else text(g, L"Every original value is saved before anything changes.", x+36, y+200, w-72, 20, 13, dim, false, 4);
}
void spinner(Graphics &g){
    float cx=380+60,cy=230+66;
    Pen track(Color(255,40,34,62),4);g.DrawEllipse(&track,cx-22,cy-22,44.0f,44.0f);
    Pen arc(lavender,4);arc.SetStartCap(LineCapRound);arc.SetEndCap(LineCapRound);
    float phase=reduceMotion?0:std::fmod(tick*300.0f,360.0f);
    g.DrawArc(&arc,cx-22,cy-22,44.0f,44.0f,phase-90,100.0f);
}
Color toneColor(int tone){return tone==1?green:tone==2?red:tone==3?amber:purple;}
// In-app dialog, styled like the website cards. Replaces native message boxes.
void drawModal(Graphics &g){
    if(!modal.open)return;
    float p=lt::easeOut(progress(modal.start,220));
    SolidBrush shade(Color(BYTE(205*p),5,4,10));g.FillRectangle(&shade,0.0f,0.0f,1280.0f,800.0f);
    hits.clear();
    float w=560,bodyW=w-64;
    float bodyH=std::min(400.0f,textHeight(g,modal.body,14,bodyW)+6);
    float h=32+52+18+bodyH+32+44+28;
    float x=(1280-w)/2,y=(800-h)/2+(1-p)*18;
    float saved=gAlpha;gAlpha=p;
    glow(g,640,y+h/2,w*1.5f,h*1.6f,Color(70,124,58,237));
    card2(g,x,y,w,h,0,22);
    Color tc=toneColor(modal.tone);
    rect(g,x+32,y+32,52,52,Color(255,BYTE(tc.GetR()/5+20),BYTE(tc.GetG()/5+16),BYTE(tc.GetB()/5+30)),16);
    icon(g,modal.icon,x+46,y+46,tc,24);
    text(g,modal.title,x+100,y+32,w-132,52,21,white,true,16);
    text(g,modal.body,x+32,y+102,bodyW,bodyH,14,Color(255,206,200,228));
    float by=y+h-28-44,bx=x+w-32;
    float pw=std::max(150.0f,textWidth(g,modal.primary,14,true)+56);
    bx-=pw;button(g,modal.primary,bx,by,pw,44,50,-1,modal.tone==2&&!modal.action?0:1);
    if(!modal.secondary.empty()){float sw=std::max(120.0f,textWidth(g,modal.secondary,14,true)+48);bx-=sw+12;button(g,modal.secondary,bx,by,sw,44,51,-1,0);}
    gAlpha=saved;
}
// Small confirmation that slides in at the bottom right after an action.
void drawToast(Graphics &g){
    if(toastText.empty()||!toastStart)return;
    float t=float(uiNow()-toastStart),total=3800;
    if(t>=total)return;
    float in=reduceMotion?1:lt::easeOut(std::clamp(t/260,0.0f,1.0f));
    float out=t>total-360?(total-t)/360:1;
    float a=std::min(in,out);
    float w=std::min(560.0f,textWidth(g,toastText,13.5f,true)+92),h=50,x=(kLeft+kRight)/2-w/2,y=64-(1-in)*22;
    float saved=gAlpha;gAlpha=a;
    Color tc=toneColor(toastKind);
    glow(g,x+w/2,y+h/2,w*1.2f,h*2.4f,Color(60,tc.GetR(),tc.GetG(),tc.GetB()));
    rect(g,x,y,w,h,Color(255,22,19,36),16);
    outlineRect(g,x+.5f,y+.5f,w-1,h-1,Color(255,BYTE(tc.GetR()/2+20),BYTE(tc.GetG()/2+16),BYTE(tc.GetB()/2+30)),1,16);
    rect(g,x+14,y+13,28,28,Color(255,BYTE(tc.GetR()/5+20),BYTE(tc.GetG()/5+16),BYTE(tc.GetB()/5+30)),14);
    icon(g,toastKind==2?IcShield:toastKind==3?IcShield:IcCheck,x+20,y+19,tc,16);
    text(g,toastText,x+54,y,w-68,h,13.5f,white,true,4|16);
    // Shrinking timer line.
    float left=std::clamp(1-t/total,0.0f,1.0f);
    rect(g,x+16,y+h-4,(w-32)*left,2,Color(160,tc.GetR(),tc.GetG(),tc.GetB()),1);
    gAlpha=saved;
}
// <<<UI-END>>>

void invalidateAnimation(HWND hwnd) {
    if(hoverStart){refresh(hwnd);return;}
    if (listVisible && listScroll.moving()) invalidateList(hwnd);
    if (transitionStart) invalidateRegion(hwnd, RectF(245, kHeader+1, 1035, 800-kHeader-1));
    else if (busy) invalidateRegion(hwnd, RectF(400, 260, 80, 80));
}
bool powerSupported(const Tweak &t) {
    if (t.kind != Kind::Power) return true;
    auto target = desired(t);
    for (ULONG i = 0;; ++i) {
        GUID guid{};
        DWORD n = sizeof(guid);
        DWORD rc = PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, i, (UCHAR *)&guid, &n);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) return false;
        if (memcmp(&guid, target.data, sizeof(guid)) == 0) return true;
    }
    return false;
}
std::wstring powerHeadline(const Tweak &t,const SystemInfo &sys){
    std::wstring cpu=sys.cpu.empty()?L"your CPU":sys.cpu;
    switch(t.value){
    case 3:return L"A performance plan built for your "+cpu;
    case 10:return sys.laptop?L"Half your cores stay awake on AC":L"All "+std::to_wstring(sys.cores)+L" threads stay unparked";
    case 7:return sys.laptop?L"Faster clock ramp-up on AC":L"Maximum-performance energy preference";
    case 8:return sys.laptop?L"Turbo boost kept on for your CPU":L"Aggressive turbo on your "+cpu;
    case 9:return L"No PCIe wake-up delay for your "+(sys.gpu.empty()?std::wstring(L"GPU"):sys.gpu);
    case 11:return L"USB input devices never suspended on AC";
    }
    return t.summary;
}
std::vector<int> detect(SystemInfo &sys) {
    std::vector<int> out(kMaxTweakId+1);
    for (const auto &t : catalog()) {
        if (!automatic(t)) continue;
        if (t.legacy) { out[t.id] = hasBackup(t) ? 1 : 3; continue; }
        try {
            if (t.kind == Kind::Registry) {
                auto plan = planFor(t, sys);
                sys.headline[t.id] = plan.headline.empty() ? std::wstring(t.summary) : plan.headline;
                bool admin = reg::needsAdmin(plan);
                sys.admin[t.id] = admin;
                std::wstring s;
                if (!plan.note.empty()) s += plan.note + L"\n\n";
                s += L"Values on this PC (current → new)\n" + reg::describe(plan);
                if (t.restart) s += L"\nTakes full effect after a restart.";
                sys.tailoring[t.id] = s;
                out[t.id] = !sys.windowsSupported ? 3 : reg::allMatch(plan) ? 2 : 1;
                continue;
            }
            if(t.kind==Kind::AdaptivePower){
                sys.headline[t.id]=powerHeadline(t,sys);
                sys.tailoring[t.id]=adaptive::describe(t.id,t.value);
                out[t.id]=sys.windowsSupported?adaptive::status(t.id,t.value):3;
                if(out[t.id]==3&&adaptive::has(t.id))sys.tailoring[t.id]=L"A previous power change has a recovery record. Restore it in Undo before applying this policy again.\n\n"+sys.tailoring[t.id];
                continue;
            }
            sys.headline[t.id]=t.summary;
            if (!sys.windowsSupported || !powerSupported(t)) { out[t.id] = 3; continue; }
            auto current = readState(t);
            out[t.id] = same(current, desired(t)) ? 2 : 1;
        } catch (const reg::Unavailable &e) {
            sys.tailoring[t.id] = L"Not available on this PC\n" + e.reason;
            sys.headline[t.id] = e.reason;
            out[t.id] = 3;
        } catch (const std::exception&e) {
            if(t.kind==Kind::AdaptivePower||t.kind==Kind::Registry)sys.tailoring[t.id]=L"Not available on this PC\n"+wide(e.what());
            out[t.id] = 3;
        }
    }
    return out;
}
void startWork(int op, std::function<void(Result &)> work) {
    if (busy.exchange(true)) return;
    listScroll.settle();
    if (worker.joinable()) worker.join();
    currentOperation = op;
    ShowWindow(keyEdit, SW_HIDE);
    stage = op == 1   ? L"Verifying your license"
            : op == 2 ? L"Matching tweaks to your hardware"
            : op == 5 ? L"Measuring connectivity"
            : op == 7 ? L"Applying and verifying each change"
            : op == 4 ? L"Putting your original values back"
                      : L"Working on your request";
    notice = stage;
    transitionStart = 0;
    animationTimer(window);
    refresh(window);
    worker = std::thread([op, work]() {
        auto r = new Result;
        r->op = op;
        try {
            work(*r);
            r->success = true;
        } catch (const std::exception &e) {
            r->message = wide(e.what());
            r->needsAdmin = r->message.find(L"administrator") != std::wstring::npos;
        }
        PostMessageW(window, WM_WORK, 0, (LPARAM)r);
    });
}
void scan() {
    if (busy) return;
    scanPhase = 0;
    probeCount = 0;
    skipBaseline = false;
    startWork(2, [](Result &r) {
        r.info = systemScan();
        PostMessageW(window, WM_SCAN_PROGRESS, 0, 1);
        r.states = detect(r.info);
        if(freeMode){
            int matched=0;for(const auto&t:catalog())if(!t.legacy&&t.tier==2&&r.states[t.id]==1)++matched;
            r.message=L"Free scan complete · Pro matched "+std::to_wstring(matched)+L" extra performance tweaks to this PC.";
            return;
        }
        PostMessageW(window, WM_SCAN_PROGRESS, 0, 2);
        try {
            auto baseline = networkBaseline(skipBaseline, [](unsigned count) { PostMessageW(window, WM_SCAN_PROGRESS, count, 2); });
            r.baselineSummary = baseline.summary();
            r.baselineReport = baseline.detail();
            if (skipBaseline) r.baselineReport += L"\n\nMeasurement stopped early at your request.";
        } catch (const std::exception &error) {
            r.baselineSummary = L"Network measurement unavailable.";
            r.baselineReport = wide(error.what()) + L"\nSupported system settings were still scanned.";
        }
        r.message = L"Scan complete. Every tweak below is matched to this PC.";
    });
}
void positionEdit() {
    if (!keyEdit) return;
    SetWindowPos(keyEdit, nullptr, (int)(offsetX + 736 * scale), (int)(offsetY + 420 * scale),
                 (int)(352 * scale), (int)(24 * scale), SWP_NOZORDER);
    HFONT f = CreateFontW((int)(17 * scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto old = (HFONT)SendMessageW(keyEdit, WM_GETFONT, 0, 0);
    SendMessageW(keyEdit, WM_SETFONT, (WPARAM)f, TRUE);
    if (old) DeleteObject(old);
    ShowWindow(keyEdit, logged || recovery || busy || modal.open ? SW_HIDE : SW_SHOW);
}
void openModal(std::wstring title, std::wstring body, std::wstring primary, std::function<void()> action,
               std::wstring secondary = L"Cancel", int tone = 0, int ic = IcBolt) {
    modal.open = true;
    modal.title = std::move(title); modal.body = std::move(body); modal.primary = std::move(primary);
    modal.secondary = std::move(secondary); modal.action = std::move(action); modal.tone = tone; modal.icon = ic;
    modal.start = uiNow();
    keyboardFocus = hover = -1;
    positionEdit();
    animationTimer(window);
    refresh(window);
}
void infoModal(std::wstring title, std::wstring body, int tone = 0, int ic = IcCheck) {
    openModal(std::move(title), std::move(body), L"Got it", nullptr, L"", tone, ic);
}
void closeModal(bool accept) {
    if (!modal.open) return;
    auto action = accept ? modal.action : nullptr;
    modal = Modal{};
    positionEdit();
    refresh(window);
    if (action) action();
}
void toast(std::wstring text, int kind = 1) {
    toastText = std::move(text); toastKind = kind; toastStart = uiNow();
    animationTimer(window);
    InvalidateRect(window, nullptr, FALSE);
}
void maskKey(){revealKey=false;SendMessageW(keyEdit,EM_SETPASSWORDCHAR,L'●',0);}
void login() {
    maskKey();
    keyError=false;
    if (!license->configured()) {
        keyError=true;
        notice = L"Pro activation isn't set up in this build yet. Continue with Free, or get a key at lunartweaks.se.";
        refresh(window);
        return;
    }
    bool polar = license->polar();
    wchar_t key[200];
    GetWindowTextW(keyEdit, key, 200);
    std::wstring input = key;
    input.erase(std::remove_if(input.begin(), input.end(), [](wchar_t c) { return iswspace(c); }), input.end());
    if (!polar) for (auto &c : input) c = towupper(c);
    if (input.empty()) {
        keyError=true;
        notice = polar ? L"Paste the Pro key from your Polar receipt email, or continue with Free." : L"Paste your Pro license key, or continue with Free.";
        refresh(window);
        return;
    }
    startWork(1, [input, polar](Result &r) {
        license->activate(utf8(input));
        if (!polar) { try { saveKey(input); } catch (...) { r.message = L"Activated; this Windows profile could not remember the key."; } }
    });
    SecureZeroMemory(key, sizeof(key));
}
// Signs straight back in with a remembered Polar key (no typing on later launches).
void resumeLicense() {
    if (busy || !license || !license->polar() || !license->remembered()) return;
    startWork(1, [](Result &) { license->resume(); });
}
void openWebsite(){
    ShellExecuteW(window,L"open",kWebsite,nullptr,nullptr,SW_SHOWNORMAL);
}
void relaunchElevated(){
    wchar_t path[MAX_PATH];
    if(!GetModuleFileNameW(nullptr,path,MAX_PATH))return;
    std::wstring args=freeMode?L"--free":(logged&&!preview?L"--activate":L"");
    SHELLEXECUTEINFOW sei{};
    sei.cbSize=sizeof(sei);sei.fMask=SEE_MASK_NOCLOSEPROCESS;sei.hwnd=window;sei.lpVerb=L"runas";
    sei.lpFile=path;sei.lpParameters=args.c_str();sei.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&sei)){
        bool cancelled=GetLastError()==ERROR_CANCELLED;
        toast(cancelled?L"Administrator restart cancelled":L"Windows couldn't restart LunarTweaks as administrator",cancelled?3:2);
        return;
    }
    // Keep this process alive (hidden) so a local owner license service stays available.
    ShowWindow(window,SW_HIDE);
    HANDLE child=sei.hProcess;
    std::thread([child]{if(child){WaitForSingleObject(child,INFINITE);CloseHandle(child);}PostMessageW(window,WM_CLOSE,0,0);}).detach();
}
std::wstring confirmText(const Tweak &t){
    std::wstring s;
    auto h=pc.headline.find(t.id);
    s+=(h!=pc.headline.end()&&!h->second.empty()?h->second:std::wstring(t.summary))+L".\n\n";
    auto it=pc.tailoring.find(t.id);
    if(it!=pc.tailoring.end())s+=it->second+L"\n\n";
    else if(t.detail&&*t.detail)s+=std::wstring(t.detail)+L"\n\n";
    if(t.restart&&(it==pc.tailoring.end()||it->second.find(L"restart")==std::wstring::npos))s+=L"Takes full effect after a restart. ";
    s+=L"Your current values are saved first, so you can undo this any time.";
    return s;
}
std::vector<int> batchIds(int category){
    std::vector<int> ids;
    for(const auto&t:catalog()){
        if(t.legacy||!automatic(t)||t.tier>tier()||!t.recommended||stateOf(t.id)!=1)continue;
        if(category>=0&&t.category!=category)continue;
        if(t.id==41&&tier()==2)continue; // The Lunar Performance plan replaces the stock plan.
        ids.push_back(t.id);
    }
    return ids;
}
void applyBatch(const std::vector<int> &ids,const std::wstring &title){
    if(ids.empty())return;
    std::wstring list;bool restart=false,admin=false;int shown=0;
    for(int id:ids){auto t=findTweak(id);if(!t)continue;if(shown++<14)list+=L"•  "+std::wstring(t->name)+L"\n";restart|=t->restart;auto a=pc.admin.find(id);admin|=a!=pc.admin.end()&&a->second;}
    if(shown>14)list+=L"…and "+std::to_wstring(shown-14)+L" more\n";
    std::wstring prompt=list+L"\nEvery current value is saved first, so you can undo any of these later.";
    if(restart)prompt+=L" Some changes finish after a restart.";
    if(admin&&!pc.elevated)prompt+=L"\n\nSome of these need administrator rights. Click \"Run as admin\" first to apply the full set.";
    openModal(title,prompt,L"Apply "+std::to_wstring(shown)+L" tweak"+(shown==1?L"":L"s"),[ids]{
    SystemInfo snapshot=pc;
    startWork(7,[ids,snapshot](Result &r){
        bool pro=false;for(int id:ids)if(auto t=findTweak(id);t&&t->tier==2)pro=true;
        if(pro)authorizeEntry(2,[]{return license->authorize();});
        int ok=0;std::wstring failures;
        for(int id:ids){
            auto t=findTweak(id);if(!t)continue;
            try{apply(*t,snapshot);++ok;}
            catch(const std::exception&e){
                auto m=wide(e.what());if(m.find(L"administrator")!=std::wstring::npos)r.needsAdmin=true;
                failures+=L"•  "+std::wstring(t->name)+L": "+m+L"\n";
            }
        }
        r.info=systemScan();r.states=detect(r.info);
        r.message=std::to_wstring(ok)+L" tweak"+(ok==1?L"":L"s")+L" applied and verified";
        if(!failures.empty())r.baselineReport=L"These were skipped:\n\n"+failures;
    });
    },L"Cancel",0,IcBolt);
}
void doAction(int action, int index) {
    if (action == 99) { PostMessageW(window, WM_CLOSE, 0, 0); return; }
    if (action == 98) { ShowWindow(window, SW_MINIMIZE); return; }
    if(action==35){sound(lt::Cue::Click);toggleFullScreen();refresh(window);return;}
    if(action==34){soundEnabled=!soundEnabled;if(!soundEnabled)PlaySoundW(nullptr,nullptr,0);else sound(lt::Cue::Click);savePreferences();refresh(window);return;}
    if(action==16){
        reduceMotion=!reduceMotion;
        if(reduceMotion){listScroll.settle();updateListHits();hoverStart=0;hoverAmount=1;}
        transitionStart=0;savePreferences();animationTimer(window);refresh(window);return;
    }
    if (busy) {
        if (action == 14 && currentOperation == 2) { skipBaseline = true; refresh(window); }
        return;
    }
    if(action!=31&&action!=33&&action!=36)sound(lt::Cue::Click);
    if(action==50){closeModal(true);return;}
    if(action==51){closeModal(false);return;}
    if(action==41){openWebsite();return;}
    if(action==40){relaunchElevated();return;}
    if(action==30){maskKey();beginTransition();freeMode=true;preview=false;logged=true;recovery=false;selected=-1;scanDone=false;pc=SystemInfo{};positionEdit();scan();return;}
    if(action==31){revealKey=!revealKey;SendMessageW(keyEdit,EM_SETPASSWORDCHAR,revealKey?0:L'●',0);InvalidateRect(keyEdit,nullptr,TRUE);refresh(window);return;}
    if(action==32){maskKey();beginTransition();freeMode=false;logged=false;preview=false;recovery=false;keyError=false;notice=L"Paste your Pro key to unlock every performance tweak.";positionEdit();animationTimer(window);refresh(window);SetFocus(keyEdit);return;}
    if(action==33){SetFocus(keyEdit);return;}
    if(action==36){detailFocus=true;return;}
    if (action == 22) {
        auto t=findTweak(index);
        if(!t||!logged||recovery||t->category!=selected)return;
        listScroll.to(std::round(listScroll.position/ROW_H)*ROW_H, uiNow(), true);scrollInput=0;updateListHits();
        transitionStart = 0;
        if(inspectedTweak!=index)detailSwitch=reduceMotion?0:uiNow();
        inspectedTweak = index;
        detailScroll.reset();detailCacheDirty=true;detailFocus=false;
        refresh(window);
        return;
    }
    if (action == 20 || action == 21) {
        transitionStart = 0;
        scrollRow = action == 20 ? std::max(0, scrollRow - 1) : std::min(maxScrollRow(), scrollRow + 1);
        hover = keyboardFocus = -1;
        refresh(window);
        return;
    }
    if (action == 18) { chartMode = index; refresh(window); return; }
    if (action == 17) {
        std::wstring detail = pc.cpu.empty() ? L"Scan your PC to read hardware information."
            : L"Processor\n" + pc.cpu + L"\n" + std::to_wstring(pc.physicalCores) + L" cores / " + std::to_wstring(pc.cores) +
              L" threads\n\nGraphics\n" + pc.gpu + L"\n\nMemory\n" + std::to_wstring(pc.ram) + L" GB\n\nWindows drive\n" + pc.storage +
              L"\n\nConnection\n" + pc.network + L"\n\nSystem\n" + pc.os + (pc.laptop ? L" · laptop" : L" · desktop") +
              L"\n\nAdministrator\n" + (pc.elevated ? L"Yes" : L"No (use Run as admin for system tweaks)");
        infoModal(L"Your hardware", detail, 0, IcCpu);
        return;
    }
    if (action == 15) {
        infoModal(L"Network baseline", baselineReport.empty() ? L"Scan with Pro to record a network baseline." : baselineReport, 0, IcWifi);
        return;
    }
    if (action == 1) { login(); return; }
    if (action == 2) {
        maskKey();
        beginTransition();
        preview = true;
        freeMode=false;
        activity.clear();
        cpuUsage = memoryUsage = -1;
        lastSampleTick = 0;
        sampling = false;
        KillTimer(window, 2);
        logged = true;
        selected = -1;
        scanDone = false;
        notice = L"Design preview: sample hardware, every change disabled.";
        pc = SystemInfo{};
        pc.cpu = L"AMD Ryzen 7 7800X3D";
        pc.gpu = L"NVIDIA GeForce RTX 4070";
        pc.ram = 32; pc.cores = 16; pc.physicalCores = 8;
        pc.os = L"Design preview"; pc.network = L"Ethernet"; pc.storage = L"NVMe SSD"; pc.elevated = true;
        positionEdit();
        refresh(window);
        return;
    }
    if (action == 3) {
        beginTransition();
        if (!logged) return;
        selected = index;
        detailScroll.reset();detailCacheDirty=true;detailFocus=false;
        listScroll.reset();listCacheDirty=true;
        listVisible = false;
        inspectedTweak = -1;
        scrollRow = 0;
        recovery = false;
        hover = keyboardFocus = -1;
        refresh(window);
        return;
    }
    if (action == 4) {
        if (preview) { notice = L"Preview uses sample hardware. Continue with Free or activate Pro to scan this PC."; refresh(window); }
        else scan();
        return;
    }
    if (action == 6) {
        beginTransition();
        recovery = !logged || !recovery;
        scrollRow = 0;
        positionEdit();
        refresh(window);
        return;
    }
    if (action == 7 && license && license->polar() && tier() == 2 && !preview) {
        openModal(L"Sign out of Pro?", L"This frees the activation for this PC, so you can use your key on another computer. Your applied tweaks stay as they are, and Undo keeps working.",
                  L"Sign out", []{ startWork(8, [](Result &){ license->signOut(); }); }, L"Cancel", 3, IcKey);
        return;
    }
    if (action == 7 || action == 70) {
        beginTransition();
        preview = false;
        freeMode=false;
        sampling = false;
        KillTimer(window, 2);
        logged = false;
        recovery = false;
        scanDone = false;
        notice = L"Choose Free or activate Pro to scan this PC.";
        positionEdit();
        refresh(window);
        return;
    }
    if (action == 10) {
        auto tp = findTweak(index);
        if(!tp)return;
        auto t=*tp;
        if (preview) { notice = L"Preview mode: changes are disabled."; refresh(window); return; }
        if (tier() < t.tier) {
            std::wstring m;
            auto it=pc.headline.find(t.id);
            if(it!=pc.headline.end()&&stateOf(t.id)!=3)m+=L"On this PC: "+it->second+L".\n\n";
            int matched=countFor(-1).lockedMatched;
            m+=L"Pro unlocks all "+std::to_wstring(proTotal())+L" performance tweaks"+(scanDone&&matched?L", and "+std::to_wstring(matched)+L" of them are already matched to your hardware":std::wstring())+
               L". Every value is chosen for your exact CPU, GPU, RAM and drive, and every change can be undone.";
            openModal(std::wstring(t.name)+L" is a Pro tweak",m,L"Get Pro",[]{openWebsite();},L"Not now",0,IcSpark);
            return;
        }
        if (t.kind == Kind::Ping) {
            startWork(5, [t](Result &r) { authorizeEntry(t.tier,[]{return license->authorize();}); r.message = pingTest(); });
            return;
        }
        auto run=[t]{
        SystemInfo snapshot=pc;
        startWork(3, [t,snapshot](Result &r) {
            r.tweakId = t.id;
            authorizeEntry(t.tier,[]{return license->authorize();});
            if (t.kind == Kind::Maintenance) {
                flushDns();
                r.message = L"DNS cache cleared. It rebuilds automatically.";
            } else {
                apply(t,snapshot);
                r.message = std::wstring(t.name) + L" applied" + (t.restart?L" · restart to finish":L"");
            }
            r.info = systemScan();
            r.states = detect(r.info);
        });
        };
        if(t.kind==Kind::Maintenance)openModal(L"Flush the DNS cache?",std::wstring(t.detail),L"Flush DNS",run,L"Cancel",0,IcWifi);
        else openModal(L"Apply "+std::wstring(t.name)+L"?",confirmText(t),L"Apply tweak",run,L"Cancel",0,t.category);
        return;
    }
    if (action == 13 || action == 42) {
        if (preview) { notice = L"Changes are disabled in preview."; refresh(window); return; }
        if (!scanDone) { scan(); return; }
        auto ids=batchIds(action==42?index:-1);
        if(ids.empty()){toast(L"Everything recommended is already active",1);return;}
        applyBatch(ids,action==13?(tier()==2?L"Apply everything recommended?":L"Apply the recommended Free tweaks?")
                                  :L"Apply recommended "+std::wstring(categories[index])+L" tweaks?");
        return;
    }
    if (action == 11) {
        if (preview) return;
        auto tp = findTweak(index);
        if(!tp)return;
        auto t=*tp;
        openModal(L"Restore "+std::wstring(t.name)+L"?",L"LunarTweaks puts back the exact value you had before. Restoring never needs a license or an internet connection.",
                  L"Restore",[t]{
        startWork(4, [t](Result &r) {
            restore(t);
            r.message = std::wstring(t.name) + L" restored" + (t.restart?L" · restart to finish":L"");
            r.info = systemScan();
            r.states = detect(r.info);
        });
        },L"Cancel",0,IcUndo);
        return;
    }
    if (action == 43) {
        if (preview) return;
        openModal(L"Restore everything?",L"Every saved original value is put back. Power plans are restored newest first, and some values finish after a restart.",
                  L"Restore everything",[]{
        startWork(4,[](Result &r){
            std::vector<const Tweak*> left;for(const auto&t:catalog())if(automatic(t)&&hasBackup(t))left.push_back(&t);
            int ok=0;std::wstring last;
            for(int pass=0;pass<4&&!left.empty();++pass){
                std::vector<const Tweak*> next;
                for(auto it=left.rbegin();it!=left.rend();++it){try{restore(**it);++ok;}catch(const std::exception&e){next.push_back(*it);last=wide(e.what());}}
                std::reverse(next.begin(),next.end());left=next;
            }
            r.info=systemScan();r.states=detect(r.info);
            r.message=std::to_wstring(ok)+L" original values restored";
            if(!left.empty())r.baselineReport=std::to_wstring(left.size())+L" values could not be restored yet:\n\n"+last;
            r.needsAdmin=!left.empty()&&last.find(L"administrator")!=std::wstring::npos;
        });
        },L"Cancel",3,IcUndo);
        return;
    }
    if (action == 12) {
        beginTransition();
        recovery = true;
        notice = L"Local recovery: restore saved values without a license.";
        positionEdit();
        refresh(window);
        return;
    }
}
// Decode the original owner-supplied PNG once; scrolling never reloads or resizes it.
void loadBrandLogo(HINSTANCE instance) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(2), RT_RCDATA);
    if (!resource) throw std::runtime_error("The embedded brand logo is missing");
    DWORD length = SizeofResource(instance, resource);
    HGLOBAL loaded = LoadResource(instance, resource);
    const void *bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || !length) throw std::runtime_error("Could not read the brand logo");
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, length);
    if (!memory) throw std::runtime_error("Could not allocate the brand logo");
    void *destination = GlobalLock(memory);
    if (!destination) { GlobalFree(memory); throw std::runtime_error("Could not load the brand logo"); }
    memcpy(destination, bytes, length);
    GlobalUnlock(memory);
    IStream *stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) { GlobalFree(memory); throw std::runtime_error("Could not decode the brand logo"); }
    bool ok = false;
    {
        Image original(stream);
        if (original.GetLastStatus() == Ok) {
            brandLogo = std::make_unique<Bitmap>(256, 256, PixelFormat32bppPARGB);
            Graphics g(brandLogo.get());
            g.Clear(Color(0, 0, 0, 0));
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            ok = g.DrawImage(&original, Rect(0, 0, 256, 256)) == Ok;
        }
    }
    stream->Release();
    if (!ok) throw std::runtime_error("Could not decode the brand logo");
}
std::unique_ptr<Bitmap> loadLight(HINSTANCE instance,int id){
    auto resource=FindResourceW(instance,MAKEINTRESOURCEW(id),RT_RCDATA);
    auto loaded=resource?LoadResource(instance,resource):nullptr;
    const void* bytes=loaded?LockResource(loaded):nullptr;
    DWORD length=resource?SizeofResource(instance,resource):0;
    if(!bytes||!length)return {};
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,length);if(!memory)return {};
    void* destination=GlobalLock(memory);if(!destination){GlobalFree(memory);return {};}
    memcpy(destination,bytes,length);GlobalUnlock(memory);
    IStream* stream=nullptr;if(FAILED(CreateStreamOnHGlobal(memory,TRUE,&stream))){GlobalFree(memory);return {};}
    std::unique_ptr<Bitmap> result;
    {Image original(stream);if(original.GetLastStatus()==Ok){
        result=std::make_unique<Bitmap>(640,640,PixelFormat32bppPARGB);Graphics g(result.get());g.Clear(Color(0,0,0,0));
        ColorMatrix matrix={{{1,0,0,0,0},{0,1,0,0,0},{0,0,1,0,0},{0,0,0,.42f,0},{0,0,0,0,1}}};
        ImageAttributes attributes;attributes.SetColorMatrix(&matrix);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(&original,Rect(0,0,640,640),0,0,original.GetWidth(),original.GetHeight(),UnitPixel,&attributes);
    }}stream->Release();return result;
}
void drawFaded(Graphics &g,Bitmap *image,const RectF &dst,float sx,float sy,float sw,float sh,float a){
    if(a>=.999f){g.DrawImage(image,dst,sx,sy,sw,sh,UnitPixel);return;}
    if(a<=.001f)return;
    ColorMatrix m={{{1,0,0,0,0},{0,1,0,0,0},{0,0,1,0,0},{0,0,0,a,0},{0,0,0,0,1}}};
    ImageAttributes at;at.SetColorMatrix(&m);
    g.DrawImage(image,dst,sx,sy,sw,sh,UnitPixel,&at);
}
void prepareListCache() {
    if (!listVisible || selected < 0) return;
    int w = std::max(1, int(std::ceil(LIST_W * scale)));
    int h = std::max(1, int(std::ceil(std::max(1,pageItemCount) * ROW_H * scale)));
    if (listSurface.ensure(w, h)) listCacheDirty = true;
    if (!listCacheDirty) return;
    Graphics g(listSurface.image.get());
    g.Clear(card);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.TranslateTransform(-LIST_X * scale, -ROWS_Y * scale);
    g.ScaleTransform(scale, scale);
    staticRows=true;
    for (int i = 0; i < pageItemCount; ++i)
        if(auto t=findTweak(shownIds[i]))controlRow(g, *t, ROWS_Y + i * ROW_H);
    staticRows=false;
    g.Flush(FlushIntentionSync);
    listCacheDirty = false;
}
void drawScrollingList(Graphics &g) {
    if (!listVisible || busy || !listSurface.image) return;
    auto saved=g.Save();
    g.SetClip(RectF(LIST_X+2,ROWS_Y,LIST_W-4,ROWS_H),CombineModeIntersect);
    g.SetInterpolationMode(InterpolationModeNearestNeighbor);
    float offset=std::round(listScroll.position*scale)/scale;
    int first=std::max(0,int(offset/ROW_H)),last=std::min(pageItemCount,int(std::ceil((offset+ROWS_H)/ROW_H)));
    bool intro=introStart&&!reduceMotion&&appear(4+(last-first))<1;
    if(!intro)
        g.DrawImage(listSurface.image.get(),RectF(LIST_X,ROWS_Y,listSurface.width/scale,ROWS_H),
                    0.0f,offset*scale,float(listSurface.width),ROWS_H*scale,UnitPixel);
    for(int i=first;i<last;++i){
        auto t=findTweak(shownIds[i]);if(!t)continue;
        float a=intro?appear(4+i-first):1;
        float rowY=ROWS_Y+i*ROW_H-offset;
        if(intro)drawFaded(g,listSurface.image.get(),RectF(LIST_X,rowY+(1-a)*14,listSurface.width/scale,ROW_H),
                           0.0f,i*ROW_H*scale,float(listSurface.width),ROW_H*scale,a);
        bool flash=t->id==recentId&&recentStart&&uiNow()-recentStart<1100;
        if(t->id==inspectedTweak||t->id==hoveredTweak||(keyboardFocus==listHitStart+i)||flash){
            float saved=gAlpha;gAlpha=a;
            auto st=g.Save();g.TranslateTransform(0,(1-a)*14);
            rect(g,ROW_X-2,rowY,ROW_W-8,ROW_H,card,0);
            if(flash){float f=1-float(uiNow()-recentStart)/1100;glow(g,ROW_X+ROW_W-62,rowY+36,220,90,Color(BYTE(110*f),122,214,160));}
            controlRow(g,*t,rowY);
            g.Restore(st);gAlpha=saved;
        }
    }
    if(listScroll.limit>0){
        rect(g,SB_X,ROWS_Y+8,4,ROWS_H-16,divider,2);
        rect(g,SB_X,scrollThumbY(),4,scrollThumbHeight(),draggingScroll?purple:dim,2);
    }
    g.Restore(saved);
}
void prepareDetailCache(){
    if(!listVisible||busy)return;
    if(detailSurface.ensure(std::max(1,int(std::ceil(DC_W*scale))),std::max(1,int(std::ceil(detailContentHeight*scale)))))detailCacheDirty=true;
    if(!detailCacheDirty)return;
    Graphics g(detailSurface.image.get());g.Clear(card);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.TranslateTransform(-DC_X*scale,-DC_Y*scale);g.ScaleTransform(scale,scale);
    drawDetailContent(g);g.Flush(FlushIntentionSync);detailCacheDirty=false;
}
void drawDetail(Graphics&g){
    if(!listVisible||busy||!detailSurface.image)return;
    auto saved=g.Save();g.SetClip(RectF(DC_X,DC_Y,DC_W,DC_H),CombineModeIntersect);
    float a=std::min(appear(3),lt::easeOut(progress(detailSwitch,220)));
    drawFaded(g,detailSurface.image.get(),RectF(DC_X,DC_Y+(1-a)*10,detailSurface.width/scale,DC_H),0.0f,
        std::round(detailScroll.position*scale),float(detailSurface.width),DC_H*scale,a);
    if(detailLimit>0){float thumb=std::max(28.0f,DC_H*DC_H/detailContentHeight);
        rect(g,DC_X+DC_W-5,DC_Y,3,DC_H,divider,1.5f);
        rect(g,DC_X+DC_W-5,DC_Y+(DC_H-thumb)*detailScroll.position/detailLimit,3,thumb,draggingDetail?purple:dim,1.5f);}
    g.Restore(saved);
}
void drawMotion(Graphics &g) {
    if (!modal.open) { drawScrollingList(g); drawDetail(g); }
    if (busy) spinner(g);
    else if (logged && !recovery && selected < 0 && !modal.open) drawActivity(g);
    drawToast(g);
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) { EndPaint(hwnd, &ps); return; }
    if (movingWindow && frameSurface.dc) {
        if (w == frameSurface.width && h == frameSurface.height)
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, frameSurface.dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        else {
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchBlt(dc, 0, 0, w, h, frameSurface.dc, 0, 0, frameSurface.width, frameSurface.height, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return;
    }
    try {
        bool changed = sceneSurface.ensure(w, h);
        frameSurface.ensure(w, h);
        scale = std::min(w / 1280.0f, h / 800.0f);
        offsetX = (w - 1280 * scale) / 2;
        offsetY = (h - 800 * scale) / 2;
        if (changed || sceneDirty) {
            Graphics g(sceneSurface.image.get());
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.Clear(Color(0, 0, 0, 0));
            g.TranslateTransform(offsetX, offsetY);
            g.ScaleTransform(scale, scale);
            hits.clear();
            listVisible = false;
            listHitStart = -1;
            if(changed){listCacheDirty = true;detailCacheDirty=true;}
            buildingScene = true;
            header(g);
            if (!logged && !recovery) loginPage(g);
            else {
                sidebar(g);
                if (recovery) recoveryPage(g);
                else workspace(g);
                text(g, notice, kLeft, 770, kWidth, 22, 12.5f, dim, false, 4|16);
            }
            if (busy) busyOverlay(g);
            else drawModal(g);
            if (keyboardFocus >= 0 && keyboardFocus < (int)hits.size()) {
                auto &focus = hits[keyboardFocus];
                if (focus.action >= 0) outlineRect(g, focus.r.X + 1, focus.r.Y + 1, focus.r.Width - 2, focus.r.Height - 2, purple, 2, 10);
            }
            buildingScene = false;
            g.Flush(FlushIntentionSync);
            sceneDirty = false;
        }
        prepareListCache();prepareDetailCache();
        bool backgroundChanged=ambientSurface.ensure(w,h);
        auto now=uiNow();
        bool foregroundMotion=listScroll.moving()||detailScroll.moving()||transitionStart||hoverStart;
        if(backgroundChanged||!ambientTick||(!reduceMotion&&!foregroundMotion&&now-ambientTick>=50)){
            if(ambientTick&&!backgroundChanged&&!reduceMotion)ambientPhase+=std::min(0.1f,float(now-ambientTick)/1000);
            ambientTick=now;
            Graphics bg(ambientSurface.image.get());bg.Clear(bgColor);
            bg.TranslateTransform(offsetX,offsetY);bg.ScaleTransform(scale,scale);
            bg.SetInterpolationMode(InterpolationModeBilinear);paintBackground(bg);bg.Flush(FlushIntentionSync);
        }
        int x = ps.rcPaint.left, y = ps.rcPaint.top;
        int width = ps.rcPaint.right - x, height = ps.rcPaint.bottom - y;
        BitBlt(frameSurface.dc, x, y, width, height, ambientSurface.dc, x, y, SRCCOPY);
        BLENDFUNCTION foreground{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        AlphaBlend(frameSurface.dc,x,y,width,height,sceneSurface.dc,x,y,width,height,foreground);
        {
            Graphics g(frameSurface.image.get());
            g.SetClip(Rect(x, y, width, height));
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetInterpolationMode(InterpolationModeBilinear);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            g.TranslateTransform(offsetX, offsetY);
            g.ScaleTransform(scale, scale);
            drawMotion(g);
            g.Flush(FlushIntentionSync);
        }
        if (transitionStart && transitionSurface.dc && !busy && !reduceMotion) {
            float progress = lt::easeOut(float(uiNow() - transitionStart) / 260.0f);
            if (progress < 1 && transitionSurface.width == w && transitionSurface.height == h) {
                int tx = int(offsetX + 245 * scale), ty = int(offsetY + (kHeader+1) * scale);
                int tw = int(1035 * scale), th = int((800-kHeader-1) * scale);
                int shift=int((1-progress)*10*scale);
                if(shift>0){
                    BitBlt(frameSurface.dc,tx+shift,ty,tw-shift,th,frameSurface.dc,tx,ty,SRCCOPY);
                    BitBlt(frameSurface.dc,tx,ty,shift,th,transitionSurface.dc,tx,ty,SRCCOPY);
                }
                BLENDFUNCTION blend{AC_SRC_OVER, 0, BYTE((1 - progress) * 255), 0};
                AlphaBlend(frameSurface.dc, tx, ty, tw, th, transitionSurface.dc, tx, ty, tw, th, blend);
            } else transitionStart = 0;
        }
        BitBlt(dc, x, y, width, height, frameSurface.dc, x, y, SRCCOPY);
        animationTimer(hwnd);
    } catch (...) {
        buildingScene = false;
        sceneDirty = true;
        FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    EndPaint(hwnd, &ps);
}
LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if(m==WM_KEYDOWN&&(w==VK_F11||(w==VK_ESCAPE&&fullScreen))){if(!(l&(1LL<<30)))doAction(35,-1);return 0;}
    if((m==WM_SYSCHAR&&w==VK_RETURN)||(m==WM_CHAR&&w==VK_ESCAPE))return 0;
    if(m==WM_SYSKEYDOWN&&w==VK_RETURN){if(!(l&(1LL<<30)))doAction(35,-1);return 0;}
    if(m==WM_SETFOCUS||m==WM_KILLFOCUS){keyFocused=m==WM_SETFOCUS;refresh(window);}
    if(m==WM_CHAR&&keyError){keyError=false;notice=L"";refresh(window);}
    if (m == WM_KEYDOWN && w == VK_TAB) {
        for(int i=0;i<(int)hits.size();i++)if(hits[i].action==33)keyboardFocus=i;
        SetFocus(window);
        PostMessageW(window, WM_KEYDOWN, VK_TAB, 0);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_RETURN) { login(); return 0; }
    return DefSubclassProc(h, m, w, l);
}
LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        window = hwnd;
        frameClock.start(hwnd,WM_FRAME);
        keyEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, 0,
                                  0, 0, 0, hwnd, (HMENU)ID_KEY, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(keyEdit, EM_SETCUEBANNER, 0, (LPARAM)L"Paste your license key");
        SendMessageW(keyEdit, EM_SETLIMITTEXT, 180, 0);
        SetWindowSubclass(keyEdit, editProc, 1, 0);
        try { if (!license || !license->polar()) { auto key = loadKey(); SetWindowTextW(keyEdit, key.c_str()); } } catch (...) {}
        animationTimer(hwnd);
        SetFocus(keyEdit);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto mi = (MINMAXINFO *)l;
        if(!fullScreen)mi->ptMinTrackSize = {1152, 740};
        return 0;
    }
    case WM_SIZE: {
        transitionStart = 0;
        if (w == SIZE_MINIMIZED) { animationTimer(hwnd); return 0; }
        RECT r;
        GetClientRect(hwnd, &r);
        scale = std::min(r.right / 1280.0f, r.bottom / 800.0f);
        offsetX = (r.right - 1280 * scale) / 2;
        offsetY = (r.bottom - 800 * scale) / 2;
        if (!movingWindow) positionEdit();
        animationTimer(hwnd);
        refresh(hwnd);
        return 0;
    }
    case WM_ENTERSIZEMOVE: movingWindow = true; transitionStart = 0; animationTimer(hwnd); return 0;
    case WM_EXITSIZEMOVE: movingWindow = false; positionEdit(); animationTimer(hwnd); refresh(hwnd); return 0;
    case WM_ACTIVATEAPP:
        appActive = w != 0;ambientTick=uiNow();
        if(!appActive)PlaySoundW(nullptr,nullptr,0);
        animationTimer(hwnd);
        if (appActive && listVisible) refresh(hwnd);
        return 0;
    case WM_DISPLAYCHANGE: if(fullScreen)fitFullScreen(hwnd); refresh(hwnd); return 0;
    case WM_DPICHANGED: {
        if(fullScreen){fitFullScreen(hwnd);positionEdit();refresh(hwnd);return 0;}
        auto bounds = (RECT *)l;
        SetWindowPos(hwnd, nullptr, bounds->left, bounds->top, bounds->right - bounds->left, bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
        positionEdit();
        refresh(hwnd);
        return 0;
    }
    case WM_FRAME:
        frameClock.acknowledge();
        w=1;
        [[fallthrough]];
    case WM_TIMER:
        if (w == 2 && sampling) {
            sampleActivity();
            if (!busy) invalidateRegion(hwnd, RectF(ACT_X, ACT_Y, ACT_W, ACT_H));
            return 0;
        }
        if (w == 1 && !movingWindow && !IsIconic(hwnd) && appActive) {
            if(scrollInput&&!draggingScroll&&uiNow()-scrollInput>=120){
                listScroll.to(std::round(listScroll.target/ROW_H)*ROW_H,uiNow(),reduceMotion);scrollInput=0;
            }
            if(listVisible&&!busy&&detailScroll.advance(uiNow()))invalidateRegion(hwnd,RectF(DC_X,DC_Y,DC_W,DC_H));
            if (listVisible && !busy && listScroll.advance(uiNow())) { updateListHits(); invalidateList(hwnd); }
            tick = (uiNow() - animationStart) / 1000.0f;
            if (!reduceMotion && sceneAnimating()) sceneDirty = true;
            else if (introStart && uiNow() - introStart >= 1400 && !sceneDirty) { introStart = 0; sceneDirty = true; }
            hoverAmount=hoverStart?std::min(1.0f,float(uiNow()-hoverStart)/150):1;
            if(hoverAmount>=1&&hoverStart){hoverStart=0;refresh(hwnd);}
            invalidateAnimation(hwnd);
            if (hoverAmount < .005f && hover < 0) { hoverAmount = 0; animatedHover = -1; }
            if (hoverAmount > .995f) hoverAmount = 1;
            if(sceneDirty||frameAnimating()||(!reduceMotion&&!listScroll.moving()&&!detailScroll.moving()&&!hoverStart&&!transitionStart))InvalidateRect(hwnd,nullptr,FALSE);
            animationTimer(hwnd);
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(hwnd); return 0;
    case WM_CTLCOLOREDIT: {
        SetTextColor((HDC)w, RGB(240, 236, 252));
        SetBkColor((HDC)w, RGB(10,8,18));
        static HBRUSH b = CreateSolidBrush(RGB(10,8,18));
        return (LRESULT)b;
    }
    case WM_MOUSEMOVE: {
        if(draggingDetail){float y=(GET_Y_LPARAM(l)-offsetY)/scale;float thumb=std::max(28.0f,DC_H*DC_H/detailContentHeight);
            detailScroll.to((y-DC_Y-detailGrab)*detailLimit/(DC_H-thumb),uiNow(),true);
            invalidateRegion(hwnd,RectF(DC_X,DC_Y,DC_W,DC_H));return 0;}
        if (draggingScroll) {
            float y = (GET_Y_LPARAM(l) - offsetY) / scale;
            moveList((y - ROWS_Y - 8 - scrollGrab) * listScroll.limit / (ROWS_H - 16 - scrollThumbHeight()), true);
            return 0;
        }
        float x = (GET_X_LPARAM(l) - offsetX) / scale, y = (GET_Y_LPARAM(l) - offsetY) / scale;
        int found = -1;
        for (int i = 0; i < (int)hits.size(); ++i)
            if (hits[i].r.Contains(x, y) && hits[i].action >= 0) found = i;
        if (found != hover) {
            bool oldList=hover>=0&&hover<(int)hits.size()&&hits[hover].action==22;
            bool newList=found>=0&&hits[found].action==22;
            hover = found;
            hoveredTweak = found >= 0 && hits[found].action == 22 ? hits[found].index : -1;
            if (found >= 0) {
                animatedHover = found;
                hoverAmount = reduceMotion ? 1.0f : 0.0f;
                hoverStart=reduceMotion||newList||hits[found].action==36?0:uiNow();
                animationTimer(hwnd);
            }
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
            if(oldList&&newList)invalidateList(hwnd);else refresh(hwnd);
        }
        SetCursor(LoadCursorW(nullptr, found >= 0 && hits[found].action < 98 && hits[found].action != 36 ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE: hover = hoveredTweak = -1;hoverStart=0; refresh(hwnd); return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        float x=(GET_X_LPARAM(l)-offsetX)/scale,y=(GET_Y_LPARAM(l)-offsetY)/scale;
        if (listVisible && !busy && !modal.open) {
            if(detailLimit>0&&RectF(DC_X+DC_W-14,DC_Y,16,DC_H).Contains(x,y)){
                float thumb=std::max(28.0f,DC_H*DC_H/detailContentHeight),top=DC_Y+(DC_H-thumb)*detailScroll.position/detailLimit;
                detailGrab=y>=top&&y<=top+thumb?y-top:thumb/2;
                draggingDetail=true;detailFocus=true;SetCapture(hwnd);
                detailScroll.to((y-DC_Y-detailGrab)*detailLimit/(DC_H-thumb),uiNow(),true);
                invalidateRegion(hwnd,RectF(DC_X,DC_Y,DC_W,DC_H));return 0;
            }
            if (listScroll.limit>0 && RectF(SB_X-6, ROWS_Y, 16, ROWS_H).Contains(x, y)) {
                float thumb = scrollThumbY(), h = scrollThumbHeight();
                scrollGrab = y >= thumb && y <= thumb + h ? y - thumb : h / 2;
                draggingScroll = true;
                SetCapture(hwnd);
                moveList((y - ROWS_Y - 8 - scrollGrab) * listScroll.limit / (ROWS_H - 16 - h), true);
                invalidateList(hwnd);
                return 0;
            }
        }
        pressedHit=-1;
        for(int i=(int)hits.size()-1;i>=0;i--)if(hits[i].action>=0&&hits[i].r.Contains(x,y)){pressedHit=i;break;}
        if(pressedHit>=0){SetCapture(hwnd);refresh(hwnd);}return 0;
    }
    case WM_CAPTURECHANGED:
        pressedHit=-1;
        draggingScroll = draggingDetail = false;
        refresh(hwnd);
        if (listVisible) invalidateList(hwnd);
        return 0;
    case WM_LBUTTONUP: {
        if(draggingDetail){draggingDetail=false;ReleaseCapture();return 0;}
        if (draggingScroll) {
            draggingScroll = false;
            moveList(std::round(listScroll.target/ROW_H)*ROW_H);
            ReleaseCapture();
            invalidateList(hwnd);
            return 0;
        }
        float x=(GET_X_LPARAM(l)-offsetX)/scale,y=(GET_Y_LPARAM(l)-offsetY)/scale;
        int pressed=pressedHit;pressedHit=-1;ReleaseCapture();
        if(pressed>=0&&pressed<(int)hits.size()){auto hit=hits[pressed];if(hit.action>=0&&hit.r.Contains(x,y))doAction(hit.action,hit.index);}
        refresh(hwnd);return 0;
    }
    case WM_MOUSEWHEEL: {
        if (modal.open) return 0;
        POINT mouse{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(hwnd,&mouse);
        float mx=(mouse.x-offsetX)/scale,my=(mouse.y-offsetY)/scale;
        if(!busy&&listVisible&&RectF(DET_X,DET_Y,DET_W,DET_H).Contains(mx,my)){
            detailFocus=true;detailScroll.to(detailScroll.wheelTarget(GET_WHEEL_DELTA_WPARAM(w),wheelLines),uiNow(),reduceMotion);invalidateRegion(hwnd,RectF(DC_X,DC_Y,DC_W,DC_H));animationTimer(hwnd);return 0;
        }
        if (!busy && listVisible && !recovery && wheelLines && RectF(LIST_X,LIST_Y,LIST_W,LIST_H).Contains(mx,my)) {
            detailFocus=false;
            moveList(listScroll.wheelTarget(GET_WHEEL_DELTA_WPARAM(w), wheelLines,ROW_H));
        } else if (!busy && recovery && wheelLines) {
            recoveryWheelDelta += GET_WHEEL_DELTA_WPARAM(w);
            int steps = recoveryWheelDelta / WHEEL_DELTA;
            recoveryWheelDelta %= WHEEL_DELTA;
            if (steps) {
                transitionStart = 0;
                scrollRow = std::clamp(scrollRow - steps, 0, maxScrollRow());
                hover = keyboardFocus = -1;
                refresh(hwnd);
            }
        }
        return 0;
    }
    case WM_SETTINGCHANGE: SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines, 0); return 0;
    case WM_NCHITTEST: {
        if(fullScreen)return HTCLIENT;
        LRESULT hit = DefWindowProcW(hwnd, msg, w, l);
        if (hit != HTCLIENT) return hit;
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(hwnd, &p);
        float x = (p.x - offsetX) / scale, y = (p.y - offsetY) / scale;
        if (y >= 0 && y < kHeader) {
            for (auto &h : hits) if (h.action >= 0 && h.r.Contains(x, y)) return HTCLIENT;
            if (x < 1188) return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_SYSCHAR: if(w==VK_RETURN)return 0; break;
    case WM_SYSKEYDOWN: if(w==VK_RETURN){if(!(l&(1LL<<30)))doAction(35,-1);return 0;} break;
    case WM_KEYDOWN:
        if(modal.open&&!busy){
            if(w==VK_ESCAPE){closeModal(false);return 0;}
            if(w==VK_RETURN){sound(lt::Cue::Click);closeModal(true);return 0;}
        }
        if(w==VK_F11||(w==VK_ESCAPE&&fullScreen)){if(!(l&(1LL<<30)))doAction(35,-1);return 0;}
        if (listVisible && !busy && (w == VK_NEXT || w == VK_PRIOR || w == VK_HOME || w == VK_END)) {
            if(detailFocus||(keyboardFocus>=0&&keyboardFocus<(int)hits.size()&&hits[keyboardFocus].action==36)){
                detailScroll.to(w==VK_HOME?0:w==VK_END?detailLimit:detailScroll.target+(w==VK_NEXT?280:-280),uiNow(),reduceMotion);
                invalidateRegion(hwnd,RectF(DC_X,DC_Y,DC_W,DC_H));animationTimer(hwnd);return 0;
            }
            moveList(w == VK_HOME ? 0 : w == VK_END ? listScroll.limit : listScroll.target + (w == VK_NEXT ? ROWS_H : -ROWS_H));
            return 0;
        }
        if (w == VK_TAB && !hits.empty()) {
            if (listVisible && listScroll.moving()) {
                listScroll.to(std::round(listScroll.position/ROW_H)*ROW_H, uiNow(), true);scrollInput=0;updateListHits();
            }
            int direction = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
            for (size_t tries = 0; tries < hits.size(); ++tries) {
                keyboardFocus = (keyboardFocus + direction + (int)hits.size()) % (int)hits.size();
                auto &hit = hits[keyboardFocus];
                if (hit.action >= 0 && (!busy || hit.action == 14 || hit.action == 98 || hit.action == 16 || hit.action == 34 || hit.action == 35))
                    break;
            }
            refresh(hwnd);
            return 0;
        }
        if ((w == VK_RETURN || w == VK_SPACE) && keyboardFocus >= 0 && keyboardFocus < (int)hits.size()) {
            auto hit = hits[keyboardFocus];
            if (hit.action >= 0) doAction(hit.action, hit.index);
            return 0;
        }
        if (w == 'I' && (GetKeyState(VK_CONTROL) & 0x8000)) { doAction(17, -1); return 0; }
        if (w == 'L' && (GetKeyState(VK_CONTROL) & 0x8000) && !logged && !busy) { SetFocus(keyEdit); return 0; }
        if (w == VK_ESCAPE && !busy) {
            if (recovery) { recovery = false; positionEdit(); }
            else if (logged && selected >= 0) doAction(3, -1);
            refresh(hwnd);
        }
        if (w == 'R' && (GetKeyState(VK_CONTROL) & 0x8000) && logged && !preview) scan();
        return 0;
    case WM_SCAN_PROGRESS:
        if (busy && currentOperation == 2) {
            probeCount = static_cast<unsigned>(w);
            scanPhase = static_cast<int>(l);
            refresh(hwnd);
        }
        return 0;
    case WM_WORK: {
        std::unique_ptr<Result> r((Result *)l);
        if (worker.joinable()) worker.join();
        busy = false;
        listCacheDirty=detailCacheDirty=true;
        animationTimer(hwnd);
        positionEdit();
        if(r->op!=6)sound(r->success?lt::Cue::Complete:lt::Cue::Error);
        bool askAdmin=false;
        if (r->success) {
            if (r->op == 1) {
                logged = true;
                selected = -1;
                preview = false;
                freeMode=false;keyError=false;
                beginTransition();
                positionEdit();
                notice = L"Pro is active on this PC. Matching every tweak to your hardware…";
                toast(L"Welcome to LunarTweaks Pro",1);
                scan();
            } else if (r->op == 8) {
                doAction(70, -1);
                toast(L"Signed out · this PC's activation is free again",1);
            } else {
                if (!r->states.empty()) {
                    pc = r->info;
                    states = r->states;
                    if (r->op == 2) {
                        SYSTEMTIME now;
                        GetLocalTime(&now);
                        wchar_t stamp[32];
                        swprintf_s(stamp, L"today %02u:%02u", now.wHour, now.wMinute);
                        lastScan = stamp;
                        if (!scanDone || selected < 0) beginTransition();
                    }
                    scanDone = true;
                }
                if (r->op == 2) { baselineReport = r->baselineReport; baselineSummary = r->baselineSummary; }
                notice = r->message;
                if (r->op == 2) {
                    int ready=0;for(const auto&t:catalog())if(!t.legacy&&automatic(t)&&t.tier<=tier()&&states[t.id]==1)++ready;
                    toast(L"Scan complete · "+std::to_wstring(ready)+L" tweaks ready for this PC",1);
                } else if (r->op == 5) {
                    notice=L"Ping test complete.";
                    infoModal(L"Ping test results", r->message, 0, IcWifi);
                } else if (r->op == 3 || r->op == 4 || r->op == 7) {
                    if (r->tweakId) { recentId = r->tweakId; recentStart = reduceMotion ? 0 : uiNow(); }
                    if (!r->baselineReport.empty()) {
                        askAdmin = r->needsAdmin;
                        if (askAdmin && !pc.elevated)
                            openModal(r->message + L", some need admin", r->baselineReport + L"\nRestart LunarTweaks as administrator to apply the rest.",
                                      L"Restart as admin", []{ relaunchElevated(); }, L"Later", 3, IcShield);
                        else infoModal(r->message, r->baselineReport, 3, IcShield);
                        askAdmin = false;
                    } else toast(r->message, 1);
                }
            }
        } else {
            notice = r->message;
            if(r->op==1){
                keyError=true;
                if(logged){logged=false;positionEdit();}
            }
            else if(r->needsAdmin&&!pc.elevated)
                openModal(L"Administrator rights needed",r->message+L"\n\nThis tweak changes a system-wide setting. Restart LunarTweaks as administrator to apply it.",
                          L"Restart as admin",[]{ relaunchElevated(); },L"Later",3,IcShield);
            else infoModal(L"That didn't work", r->message, 2, IcShield);
        }
        refresh(hwnd);
        (void)askAdmin;
        return 0;
    }
    case WM_CLOSE:
        if (busy) { notice = L"Please wait for the current operation to finish."; return 0; }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        frameClock.stop();
        PlaySoundW(nullptr,nullptr,0);
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR command, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    GdiplusStartupInput input;
    if (GdiplusStartup(&gdip, &input, nullptr) != Ok) return 1;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&cc);
    try {
        uiFontFamily = std::make_unique<FontFamily>(L"Segoe UI Variable Text");
        if (uiFontFamily->GetLastStatus() != Ok) uiFontFamily = std::make_unique<FontFamily>(L"Segoe UI");
        embeddedFonts=std::make_unique<PrivateFontCollection>();
        bool fontsReady=true;
        for(int id:{3,4,5}){
            auto resource=FindResourceW(instance,MAKEINTRESOURCEW(id),RT_RCDATA);
            auto loaded=resource?LoadResource(instance,resource):nullptr;
            auto data=loaded?LockResource(loaded):nullptr;
            if(!data||embeddedFonts->AddMemoryFont(data,SizeofResource(instance,resource))!=Ok)fontsReady=false;
        }
        if(fontsReady){
            auto family=std::make_unique<FontFamily>(L"Lunar Body",embeddedFonts.get());
            if(family->GetLastStatus()==Ok)uiFontFamily=std::move(family);
            auto heading=std::make_unique<FontFamily>(L"Lunar Display",embeddedFonts.get());
            if(heading->GetLastStatus()==Ok)headingFontFamily=std::move(heading);
        }
        clickWave=lt::cueWave(lt::Cue::Click);completeWave=lt::cueWave(lt::Cue::Complete);errorWave=lt::cueWave(lt::Cue::Error);
        animationStart = uiNow();
        groupedCpu = GetActiveProcessorGroupCount() > 1;
        BOOL allowAnimations = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &allowAnimations, 0)) reduceMotion = !allowAnimations;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines, 0);
        try{
            preferencesPath=dataDir()+L"\\interface.ini";
            soundEnabled=GetPrivateProfileIntW(L"Interface",L"Sound",1,preferencesPath.c_str())!=0;
            reduceMotion=reduceMotion||GetPrivateProfileIntW(L"Interface",L"ReduceMotion",0,preferencesPath.c_str())!=0;
        }catch(...){preferencesPath.clear();}
        loadBrandLogo(instance);
        violetLight=loadLight(instance,6);blueLight=loadLight(instance,7);
        license = std::make_unique<Licensing>();
        pc.elevated = processElevated();
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = L"LunarTweaksWindow";
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"LunarTweaks",
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1296, 840, nullptr, nullptr, instance, nullptr);
        if (!hwnd) throw std::runtime_error("Could not create the application window");
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        DWORD noBorder = 0xFFFFFFFE;
        DwmSetWindowAttribute(hwnd, 34, &noBorder, sizeof(noBorder));
        DWORD round = 2;
        DwmSetWindowAttribute(hwnd, 33, &round, sizeof(round));
        ShowWindow(hwnd, show);
        UpdateWindow(hwnd);
        std::wstring args = command;
        if(args.find(L"--free")!=std::wstring::npos)doAction(30,-1);
        else if (args.find(L"--preview") != std::wstring::npos) doAction(2, -1);
        else if (license->polar() && license->remembered()) resumeLicense();
        else if (args.find(L"--activate") != std::wstring::npos && GetWindowTextLengthW(keyEdit) > 0) login();
        else { beginTransition(); refresh(hwnd); }
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (worker.joinable()) worker.join();
    } catch (const std::exception &e) {
        MessageBoxW(nullptr, wide(e.what()).c_str(), L"LunarTweaks", MB_OK | MB_ICONERROR);
    }
    frameClock.stop();
    listSurface.reset();detailSurface.reset();ambientSurface.reset();violetLight.reset();blueLight.reset();
    brandLogo.reset();
    transitionSurface.reset();
    frameSurface.reset();
    sceneSurface.reset();
    uiFonts.clear();
    uiFontFamily.reset();headingFontFamily.reset();embeddedFonts.reset();
    license.reset();
    CoUninitialize();
    GdiplusShutdown(gdip);
    return 0;
}
