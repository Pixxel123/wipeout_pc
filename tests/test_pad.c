#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"
#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif

static int capture;
static int vibration_percent=100;
static uint16_t rumble_low, rumble_high;
int gow3gpu_overlay_captures_input(void) { return capture; }
int gow3gpu_vibration_percent(void) { return vibration_percent; }

static bool SDLCALL record_rumble(void *userdata, uint16_t low, uint16_t high) {
    (void)userdata;
    rumble_low=low;
    rumble_high=high;
    return true;
}
void runtime_user_events(void) {}
int gow3gpu_stick_deadzone_percent(void) { return 0; }
void gow3gpu_request_screenshot(void) {}
int gow3gpu_mouse_prompt_distance(void) { return 0; }
void gow3gpu_mouse_pause_key(void) {}
int gow3gpu_mouse_aim_style(void) { return 0; }
int gow3gpu_mouse_aim_sensitivity(void) { return 100; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}
static PadData read_pad(void) {
    PadData d;
    assert(pad_read_state(1,&d)==0);
    return d;
}
/* One finger in the middle of the left (480) or right (1440) half of the touchpad. */
static int touch_at(const PadData *d, int x) {
    return d->touch_count==1 && d->touches[0].x==x && d->touches[0].y==471;
}
static void pump(void) { SDL_UpdateJoysticks(); SDL_UpdateGamepads(); }
/* A virtual gamepad connecting (a controller waking up) and going away (one that sleeps). */
typedef struct { SDL_JoystickID id; SDL_Joystick *joystick; } TestPad;
static TestPad plug(const SDL_VirtualJoystickDesc *desc) {
    TestPad pad={SDL_AttachVirtualJoystick(desc),NULL};
    assert(pad.id!=0);
    pad.joystick=SDL_OpenJoystick(pad.id);
    assert(pad.joystick);
    pump();
    return pad;
}
static void unplug(TestPad *pad) {
    SDL_CloseJoystick(pad->joystick);
    pad->joystick=NULL;
    assert(SDL_DetachVirtualJoystick(pad->id));
    pump();
}
static void press(TestPad *pad, SDL_GamepadButton button, bool down) {
    assert(SDL_SetJoystickVirtualButton(pad->joystick,button,down));
    pump();
}
/* Player 1's read once the stand-in is due its next look for player 1's own gamepad. */
static PadData read_pad_later(void) { usleep(AWAY_CHECK_US+20000); return read_pad(); }
static SDL_JoystickID player1(void) { return gamepad ? SDL_GetGamepadID(gamepad) : 0; }
static SDL_JoystickID player2(void) { return gamepad2 ? SDL_GetGamepadID(gamepad2) : 0; }

int main(void) {
    const char *tmp=getenv("TMPDIR") ? getenv("TMPDIR") : getenv("TEMP") ? getenv("TEMP") : "/tmp";
    char path[1024];
    snprintf(path,sizeof(path),"%s/gow3-pad-test-XXXXXX",tmp);
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("GOW3_PAD_FILE",path,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controllers are gamepads, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189,0x1d50/0x618a");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);

    /* A click without a touch surface (Back, Tab, Backspace, the pad file) plays like a finger:
     * the touch comes first, the button CLICK_LEAD_US later, and the touch stays CLICK_TAIL_US
     * after the release. God of War III opens photo mode on the release of a left click with
     * the finger still down; a touch that appears and vanishes with the button is ignored. */
    inject(path,"cross l3 touchpad_left");
    PadData data=read_pad();
    assert((data.buttons & (BTN_CROSS|BTN_L3))==(BTN_CROSS|BTN_L3));
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    inject(path,"");
    data=read_pad();
    assert(data.buttons==0 && touch_at(&data,480)); /* released: the finger lingers */
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* A tap shorter than the lead still clicks, for CLICK_HOLD_US. */
    inject(path,"touchpad_right");
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    inject(path,"");
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(60000);
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* L1 + the right click is a left click (photo mode on a pad without a touchpad). The side
     * is chosen when the click starts and kept until it ends. */
    inject(path,"l1 touchpad_right");
    data=read_pad();
    assert((data.buttons & BTN_L1) && touch_at(&data,480));
    inject(path,"touchpad_right");
    data=read_pad();
    assert(!(data.buttons & BTN_L1) && touch_at(&data,480));
    inject(path,"");
    data=read_pad(); /* the release is seen here: the tail starts */
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* The settings menu takes the input and ends a click; the source starts a new one after. */
    inject(path,"touchpad_right");
    usleep(60000);
    data=read_pad();
    assert(touch_at(&data,1440));
    capture=1;
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);
    capture=0;
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    inject(path,"");
    data=read_pad(); /* the release is seen here: the tail starts */
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="gow3 test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    desc.Rumble=record_rumble;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    /* A real touchpad: the fingers and the click go to the game as they are. */
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    pump();
    data=read_pad();
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    const uint8_t motors[]={255,128};
    assert(pad_vibration(1,motors)==0);
    assert(rumble_low==65535 && rumble_high==32896);
    vibration_percent=50;
    assert(pad_vibration(1,motors)==0);
    assert(rumble_low==32767 && rumble_high==16448);
    vibration_percent=0;
    assert(pad_vibration(1,motors)==0);
    assert(rumble_low==0 && rumble_high==0);
    vibration_percent=100;
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==0);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==1);
    capture=1;
    data=read_pad();
    assert(data.touch_count==0 && data.buttons==0);
    capture=0;
    /* The click without a finger is played as a left click. */
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    pump();
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    pump();
    data=read_pad();
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);
    /* Back (Share, View) is the right click: the pause menu. With L1 it is the left click. */
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,true));
    pump();
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,false));
    pump();
    data=read_pad();
    usleep(220000);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,true));
    pump();
    data=read_pad();
    assert((data.buttons & BTN_L1) && !(data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & (BTN_L1|BTN_TOUCHPAD))==(BTN_L1|BTN_TOUCHPAD) && touch_at(&data,480));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,false));
    pump();
    data=read_pad();
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);
    /* Split-screen: a second gamepad logs user 2 in; its pad reads only that gamepad. */
    assert(!runtime_pad_second_user());
    desc.name="gow3 test controller 2";
    SDL_JoystickID id2=SDL_AttachVirtualJoystick(&desc);
    assert(id2!=0);
    SDL_Joystick *joystick2=SDL_OpenJoystick(id2);
    assert(joystick2);
    pump();
    assert(runtime_pad_second_user());
    assert(pad_open(2,0,0,NULL)==2 && pad_open(2,0,0,NULL)==ERR_ALREADY_OPENED);
    assert(SDL_SetJoystickVirtualButton(joystick2,SDL_GAMEPAD_BUTTON_SOUTH,true));
    assert(SDL_SetJoystickVirtualAxis(joystick2,SDL_GAMEPAD_AXIS_LEFTX,-32768));
    pump();
    PadData second;
    assert(pad_read_state(2,&second)==0);
    assert(second.connected && (second.buttons & BTN_CROSS) && second.left_x==0);
    data=read_pad();
    assert(!(data.buttons & BTN_CROSS) && data.left_x==128);
    assert(SDL_SetJoystickVirtualButton(joystick2,SDL_GAMEPAD_BUTTON_SOUTH,false));
    SDL_CloseJoystick(joystick2);
    assert(SDL_DetachVirtualJoystick(id2));
    pump();
    assert(pad_read_state(2,&second)==0 && !second.connected && second.buttons==0);
    assert(!runtime_pad_second_user());
    assert(pad_close(2)==0 && pad_read_state(2,&second)==ERR_INVALID_HANDLE);
    /* A controller that sleeps (the Steam Deck): the gamepad that is left (the Deck's own
     * controls) stands in for player 1, and the controller takes player 1 back when it wakes
     * up, which SDL reports as a new gamepad. Before, player 1 stayed on the stand-in. */
    TestPad pad={id,joystick};
    SDL_VirtualJoystickDesc deck_desc=desc;
    deck_desc.name="gow3 test built-in controls";
    TestPad deck=plug(&deck_desc);
    data=read_pad();
    assert(!away && player1()==pad.id); /* a gamepad that connects later is not player 1's */
    unplug(&pad); /* the controller sleeps */
    data=read_pad();
    assert(away && player1()==deck.id);
    pad=plug(&desc); /* it wakes up */
    press(&pad,SDL_GAMEPAD_BUTTON_EAST,true);
    data=read_pad_later();
    assert(!away && player1()==pad.id && (data.buttons & BTN_CIRCLE));
    press(&pad,SDL_GAMEPAD_BUTTON_EAST,false);
    /* Player 1's gamepad is there: another one going away and coming back changes nothing. */
    unplug(&deck);
    data=read_pad();
    deck=plug(&deck_desc);
    data=read_pad_later();
    assert(!away && player1()==pad.id);
    /* Input on the stand-in: player 1 keeps it, and a gamepad that connects later is not player
     * 1's (a friend's controller an hour after player 1's own one died). */
    unplug(&pad);
    data=read_pad();
    assert(away && player1()==deck.id);
    press(&deck,SDL_GAMEPAD_BUTTON_SOUTH,true);
    data=read_pad();
    assert(!away && player1()==deck.id && (data.buttons & BTN_CROSS));
    press(&deck,SDL_GAMEPAD_BUTTON_SOUTH,false);
    data=read_pad();
    pad=plug(&desc);
    data=read_pad_later();
    assert(!away && player1()==deck.id);
    /* Player 2's gamepad stands in while the game does not read it (one player): player 2
     * keeps it when player 1's own one is back, and gives it up when player 1 plays on it. */
    assert(runtime_pad_second_user() && player2()==pad.id);
    unplug(&deck);
    data=read_pad();
    assert(away && gamepad && gamepad==gamepad2);
    deck=plug(&deck_desc);
    data=read_pad_later();
    assert(!away && player1()==deck.id && player2()==pad.id);
    unplug(&deck);
    data=read_pad();
    assert(away && gamepad && gamepad==gamepad2);
    press(&pad,SDL_GAMEPAD_BUTTON_SOUTH,true);
    data=read_pad();
    assert(!away && player1()==pad.id && !gamepad2 && (data.buttons & BTN_CROSS));
    press(&pad,SDL_GAMEPAD_BUTTON_SOUTH,false);
    data=read_pad();
    assert(!runtime_pad_second_user());
    /* Split-screen, the game reading player 2's pad: it never stands in (it would move both
     * players); player 1 waits for its own gamepad. */
    deck=plug(&deck_desc);
    assert(runtime_pad_second_user() && player2()==deck.id);
    assert(pad_open(2,0,0,NULL)==2 && pad_read_state(2,&second)==0);
    unplug(&pad);
    data=read_pad();
    assert(away && !gamepad && data.connected);
    assert(pad_read_state(2,&second)==0 && second.connected && player2()==deck.id);
    pad=plug(&desc);
    data=read_pad();
    assert(!away && player1()==pad.id && player2()==deck.id);
    assert(pad_close(2)==0);
    /* Player 2 opens the woken controller before player 1 looks: player 1 takes it over, and
     * player 2 gets the stand-in. */
    unplug(&deck);
    assert(!runtime_pad_second_user());
    deck=plug(&deck_desc);
    unplug(&pad);
    data=read_pad();
    assert(away && player1()==deck.id && !gamepad2);
    data=read_pad_later();
    pad=plug(&desc);
    assert(runtime_pad_second_user() && player2()==pad.id);
    data=read_pad_later();
    assert(!away && player1()==pad.id && !gamepad2);
    assert(runtime_pad_second_user() && player2()==deck.id);
    unplug(&deck);
    assert(!runtime_pad_second_user());
    /* The only gamepad sleeps, and two connect: the same model is player 1's own one. */
    unplug(&pad);
    data=read_pad();
    assert(away && !gamepad && data.connected);
    SDL_VirtualJoystickDesc other_desc=desc;
    other_desc.name="gow3 other controller";
    other_desc.product_id=0x618a;
    TestPad other=plug(&other_desc);
    pad=plug(&desc);
    data=read_pad();
    assert(!away && player1()==pad.id);
    unplug(&other);
    /* GOW3_GAMEPAD=2 (#88): two sticks drop and come back together (a hub): player 1 has the
     * second one again. */
    wanted_gamepad=2;
    TestPad stick=plug(&desc);
    unplug(&pad);
    unplug(&stick);
    data=read_pad();
    assert(away && !gamepad);
    pad=plug(&desc);
    stick=plug(&desc);
    int listed=0;
    SDL_JoystickID *ids=SDL_GetGamepads(&listed);
    assert(ids && listed==2);
    const SDL_JoystickID second_listed=ids[1];
    SDL_free(ids);
    data=read_pad();
    assert(!away && player1()==second_listed);
    wanted_gamepad=1;
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    unplug(&pad);
    unplug(&stick);
    SDL_Quit();
    unlink(path);
    puts("PASS: pad ABI, staged left/right clicks, L1 chord, SDL touch coordinates, overlay capture, player 2, controller sleep and wake");
}
