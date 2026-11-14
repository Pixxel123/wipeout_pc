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
static bool SDLCALL sensors_enabled(void *userdata, bool enabled) {
    (void)userdata; (void)enabled;
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

/* Steam's virtual gamepad has a Steam handle; the test's own gamepad stands in for it. */
static SDL_JoystickID test_virtual_id;
static Uint64 test_steam_handle(SDL_Gamepad *g) {
    return SDL_GetGamepadID(g)==test_virtual_id;
}
int main(void) {
    const char *tmp=getenv("TMPDIR") ? getenv("TMPDIR") : getenv("TEMP") ? getenv("TEMP") : "/tmp";
    char path[1024];
    snprintf(path,sizeof(path),"%s/gow3-pad-test-XXXXXX",tmp);
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("GOW3_PAD_FILE",path,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189,0x28de/0x1304");
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

    /* Steam Input (#18): the real controllers it hides (Valve's and Sony's) are read for their gyro only. Player 1
     * stays on the virtual gamepad, a real one never becomes player 2, and the gyro of the real
     * controller behind player 1's gamepad reaches the game. */
#ifndef _WIN32
    setenv("SDL_GAMECONTROLLER_IGNORE_DEVICES","0x28de/0x1205,0x054c/0x0df2,0x057e/0x2009,@list,0x28DE/0x1304",1);
    setenv("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD","1",1);
    runtime_pad_steam_input();
    /* Valve's and Sony's are read; a Switch Pro and what is not a device id stay hidden. */
    assert(!strcmp(getenv("SDL_GAMECONTROLLER_IGNORE_DEVICES"),"0x057e/0x2009,@list"));
    assert(!strcmp(getenv("GOW3_STEAM_HIDDEN_PADS"),"0x28de/0x1205,0x054c/0x0df2,0x28DE/0x1304"));
    assert(hidden_count==3 && steam_listed(0x28de,0x1304) && steam_listed(0x054c,0x0df2) && !steam_listed(0x057e,0x2009));
#endif
    /* The test's gamepad stands in for Steam's virtual one: it has a Steam handle, and the real
     * controllers behind it report its device id. */
    setenv("GOW3_STEAM_HIDDEN_PADS","0x1d50/0x6189,0x28de/0x1304",1);
    runtime_pad_steam_input();
    assert(steam_reals && hidden_count==2);
    test_virtual_id=id;
    steam_handle=test_steam_handle;
    SDL_VirtualJoystickSensorDesc gyro_sensor={.type=SDL_SENSOR_GYRO,.rate=250.0f};
    SDL_VirtualJoystickDesc real_desc=desc;
    real_desc.ntouchpads=0;
    real_desc.touchpads=NULL;
    real_desc.nsensors=1;
    real_desc.sensors=&gyro_sensor;
    real_desc.Rumble=NULL;
    real_desc.SetSensorsEnabled=sensors_enabled;
    /* A real controller with another device id (a Steam Controller's receiver beside a Steam Deck)
     * is never this player's, even pressed in the same moment as its gamepad. */
    real_desc.name="gow3 test other controller";
    real_desc.vendor_id=0x28de;
    real_desc.product_id=0x1304;
    SDL_JoystickID other_id=SDL_AttachVirtualJoystick(&real_desc);
    assert(other_id!=0);
    SDL_Joystick *other=SDL_OpenJoystick(other_id);
    assert(other);
    pump();
    usleep(REAL_SCAN_US+10000);
    data=read_pad(); /* opens the real controller */
    assert(SDL_GetGamepadID(gamepad)==id);
    assert(runtime_pad_motion_only(other_id) && !runtime_pad_motion_only(id));
    assert(!runtime_pad_second_user());
    const float turn[3]={1.0f,-2.0f,0.5f};
    assert(SDL_SetJoystickVirtualButton(other,SDL_GAMEPAD_BUTTON_SOUTH,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,true));
    pump();
    data=read_pad();
    assert(data.buttons & BTN_CROSS);
    assert(SDL_SendJoystickVirtualSensorData(other,SDL_SENSOR_GYRO,SDL_GetTicksNS(),turn,3));
    pump();
    data=read_pad();
    assert(data.angular_velocity[0]==0.0f && data.angular_velocity[1]==0.0f && data.angular_velocity[2]==0.0f);
    assert(SDL_SetJoystickVirtualButton(other,SDL_GAMEPAD_BUTTON_SOUTH,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,false));
    pump();
    data=read_pad();
    /* The one real controller with the gamepad's device id is the player's, before any press. */
    real_desc.name="gow3 test real controller";
    real_desc.vendor_id=desc.vendor_id;
    real_desc.product_id=desc.product_id;
    SDL_JoystickID real_id=SDL_AttachVirtualJoystick(&real_desc);
    assert(real_id!=0);
    SDL_Joystick *real=SDL_OpenJoystick(real_id);
    assert(real);
    pump();
    usleep(REAL_SCAN_US+10000);
    data=read_pad(); /* opens the real controller */
    assert(runtime_pad_motion_only(real_id));
    assert(!runtime_pad_second_user());
    assert(SDL_SendJoystickVirtualSensorData(real,SDL_SENSOR_GYRO,SDL_GetTicksNS(),turn,3));
    pump();
    data=read_pad();
    assert(data.angular_velocity[0]==1.0f && data.angular_velocity[1]==-2.0f && data.angular_velocity[2]==0.5f);
    /* With two of that id, player 1 moves to the other one once that is the one pressed with the gamepad. */
    real_desc.name="gow3 test real controller 2";
    SDL_JoystickID real2_id=SDL_AttachVirtualJoystick(&real_desc);
    assert(real2_id!=0);
    SDL_Joystick *real2=SDL_OpenJoystick(real2_id);
    assert(real2);
    pump();
    usleep(REAL_SCAN_US+10000);
    data=read_pad();
    assert(!runtime_pad_second_user());
    assert(SDL_SetJoystickVirtualButton(real2,SDL_GAMEPAD_BUTTON_SOUTH,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,true));
    pump();
    data=read_pad();
    assert(data.buttons & BTN_CROSS);
    const float again[3]={0.25f,0.25f,0.25f}, turn2[3]={3.0f,0.0f,-1.0f};
    assert(SDL_SendJoystickVirtualSensorData(real,SDL_SENSOR_GYRO,SDL_GetTicksNS(),again,3));
    assert(SDL_SendJoystickVirtualSensorData(real2,SDL_SENSOR_GYRO,SDL_GetTicksNS(),turn2,3));
    pump();
    data=read_pad();
    assert(data.angular_velocity[0]==3.0f && data.angular_velocity[1]==0.0f && data.angular_velocity[2]==-1.0f);
    /* Both real controllers used in the same moment (two players): player 1 keeps its own. */
    assert(SDL_SetJoystickVirtualButton(real,SDL_GAMEPAD_BUTTON_EAST,true));
    assert(SDL_SetJoystickVirtualButton(real2,SDL_GAMEPAD_BUTTON_SOUTH,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,false));
    pump();
    data=read_pad();
    assert(!(data.buttons & BTN_CROSS));
    assert(data.angular_velocity[0]==3.0f && data.angular_velocity[1]==0.0f && data.angular_velocity[2]==-1.0f);
    assert(SDL_SetJoystickVirtualButton(real,SDL_GAMEPAD_BUTTON_EAST,false));
    SDL_CloseJoystick(real2);
    assert(SDL_DetachVirtualJoystick(real2_id));
    SDL_CloseJoystick(real);
    assert(SDL_DetachVirtualJoystick(real_id));
    SDL_CloseJoystick(other);
    assert(SDL_DetachVirtualJoystick(other_id));
    steam_handle=SDL_GetGamepadSteamHandle;
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    puts("PASS: pad ABI, staged left/right clicks, L1 chord, SDL touch coordinates, overlay capture, player 2, gyro behind Steam Input");
}
