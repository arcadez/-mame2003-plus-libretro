/*********************************************************************

	mame2003.c
    
    a port of mame 0.78 to the libretro API
    
*********************************************************************/    

#include <stdint.h>
#include <string/stdstring.h>
#include <libretro.h>
#include <file/file_path.h>

#include "mame.h"
#include "driver.h"
#include "state.h"
#include "log.h"
#include "input.h"
#include "inptport.h"
#include "fileio.h"


static const struct GameDriver  *game_driver;
static float              delta_samples;
int                       samples_per_frame = 0;
short*                    samples_buffer;
short*                    conversion_buffer;
int                       usestereo = 1;
int16_t                   prev_pointer_x;
int16_t                   prev_pointer_y;
unsigned                  retroColorMode;
unsigned long             lastled = 0;
int16_t                   XsoundBuffer[2048];

bool is_neogeo     = false;
bool is_stv        = false;
bool is_sf2_layout = false;

extern const struct KeyboardInfo retroKeys[];
extern int          retroKeyState[512];
extern int          retroJsState[72];
extern int16_t      mouse_x[4];
extern int16_t      mouse_y[4];
extern struct       osd_create_params videoConfig;
extern int16_t      analogjoy[4][4];
struct ipd          *default_inputs; /* pointer the array of structs with default MAME input mappings and labels */

static const char * (*content_aware_labeler)(int);;

retro_log_printf_t                 log_cb;

struct                             retro_perf_callback perf_cb;
retro_environment_t                environ_cb                    = NULL;
retro_video_refresh_t              video_cb                      = NULL;
static retro_input_poll_t          poll_cb                       = NULL;
static retro_input_state_t         input_cb                      = NULL;
static retro_audio_sample_batch_t  audio_batch_cb                = NULL;
retro_set_led_state_t              led_state_cb                  = NULL;

bool old_dual_joystick_state = false; /* used to track when this core option changes */

/******************************************************************************

  private function prototypes

******************************************************************************/
static void init_core_options(void);
void init_option(struct retro_variable *option, const char *key, const char *value);
static void update_variables(bool first_time);
static void check_system_specs(void);
const char *sf2_btn_label(int input);
const char *neogeo_btn_label(int input);
const char *generic_btn_label(int input);
const char *null_btn_labeler(int input);
const char *button_labeler(int input);
void retro_describe_buttons(void);

/******************************************************************************

  implementation of key libretro functions

******************************************************************************/

void retro_init (void)
{
  struct retro_log_callback log;
  if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
    log_cb = log.log;
  else
    log_cb = NULL;

#ifdef LOG_PERFORMANCE
  environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb);
#endif

  options.use_samples = 1;
  content_aware_labeler = &null_btn_labeler;
  check_system_specs();
}

static void check_system_specs(void)
{
   /* TODO - set variably */
   /* Midway DCS - Mortal Kombat/NBA Jam etc. require level 9 */
   unsigned level = 10;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_set_environment(retro_environment_t cb)
{
  environ_cb = cb;
}

static void init_core_options(void)
{
  int default_index   = 0;
  int effective_index = 0;
  static struct retro_variable default_options[OPT_end + 1];    /* need the plus one for the NULL entries at the end */
  static struct retro_variable effective_options[OPT_end + 1]; 

  init_option(&default_options[OPT_FRAMESKIP],           APPNAME"_frameskip",           "Frameskip; 0|1|2|3|4|5");
  init_option(&default_options[OPT_INPUT_INTERFACE],     APPNAME"_input_interface",     "Input interface; retropad|mame_keyboard|simultaneous");
  init_option(&default_options[OPT_RETROPAD_LAYOUT],     APPNAME"_retropad_layout",     "RetroPad Layout; modern|SNES|MAME classic");
#if defined(__IOS__)
  init_option(&default_options[OPT_MOUSE_DEVICE],        APPNAME"_mouse_device",        "Mouse Device; pointer|mouse|disabled");
#else
  init_option(&default_options[OPT_MOUSE_DEVICE],        APPNAME"_mouse_device",        "Mouse Device; mouse|pointer|disabled");
#endif
  init_option(&default_options[OPT_CROSSHAIR_ENABLED],   APPNAME"_crosshair_enabled",   "Show Lightgun crosshair; enabled|disabled");
  init_option(&default_options[OPT_SKIP_DISCLAIMER],     APPNAME"_skip_disclaimer",     "Skip Disclaimer; disabled|enabled");
  init_option(&default_options[OPT_SKIP_WARNINGS],       APPNAME"_skip_warnings",       "Skip Warnings; disabled|enabled");    
  init_option(&default_options[OPT_DISPLAY_SETUP],       APPNAME"_display_setup",       "Display MAME menu; disabled|enabled");
  init_option(&default_options[OPT_BRIGHTNESS],          APPNAME"_brightness",
                                                                                        "Brightness; 1.0|0.2|0.3|0.4|0.5|0.6|0.7|0.8|0.9|1.1|1.2|1.3|1.4|1.5|1.6|1.7|1.8|1.9|2.0");
  init_option(&default_options[OPT_GAMMA],               APPNAME"_gamma",
                                                                                        "Gamma correction; 1.2|0.5|0.6|0.7|0.8|0.9|1.1|1.2|1.3|1.4|1.5|1.6|1.7|1.8|1.9|2.0");
  init_option(&default_options[OPT_BACKDROP],            APPNAME"_enable_backdrop",     "EXPERIMENTAL: Use Backdrop artwork (Restart); disabled|enabled");
  init_option(&default_options[OPT_NEOGEO_BIOS],         APPNAME"_neogeo_bios", 
                                                                                        "Specify Neo Geo BIOS (Restart); default|euro|euro-s1|us|us-e|asia|japan|japan-s2|unibios33|unibios20|unibios13|unibios11|unibios10|debug|asia-aes");
  init_option(&default_options[OPT_STV_BIOS],            APPNAME"_stv_bios",            "Specify Sega ST-V BIOS (Restart); default|japan|japana|us|japan_b|taiwan|europe");    
  init_option(&default_options[OPT_SHARE_DIAL],          APPNAME"_dialsharexy",         "Share 2 player dial controls across one X/Y device; disabled|enabled");
  init_option(&default_options[OPT_DUAL_JOY],            APPNAME"_dual_joysticks",      "Dual Joystick Mode (Players 1 & 2); disabled|enabled");
  init_option(&default_options[OPT_RSTICK_BTNS],         APPNAME"_rstick_to_btns",      "Right Stick to Buttons; enabled|disabled");
  init_option(&default_options[OPT_TATE_MODE],           APPNAME"_tate_mode",           "TATE Mode; disabled|enabled");
  init_option(&default_options[OPT_VECTOR_RESOLUTION],   APPNAME"_vector_resolution_multiplier", 
                                                                                        "EXPERIMENTAL: Vector resolution multiplier (Restart); 1|2|3|4|5|6");
  init_option(&default_options[OPT_VECTOR_ANTIALIAS],    APPNAME"_vector_antialias",    "EXPERIMENTAL: Vector antialias; disabled|enabled");
  init_option(&default_options[OPT_VECTOR_TRANSLUCENCY], APPNAME"_vector_translucency", "Vector translucency; enabled|disabled");
  init_option(&default_options[OPT_VECTOR_BEAM],         APPNAME"_vector_beam_width",   "EXPERIMENTAL: Vector beam width; 1|2|3|4|5");
  init_option(&default_options[OPT_VECTOR_FLICKER],      APPNAME"_vector_flicker",      "Vector flicker; 20|0|10|20|30|40|50|60|70|80|90|100");
  init_option(&default_options[OPT_VECTOR_INTENSITY],    APPNAME"_vector_intensity",    "Vector intensity; 1.5|0.5|1|2|2.5|3");
  init_option(&default_options[OPT_SKIP_CRC],            APPNAME"_skip_rom_verify",     "EXPERIMENTAL: Skip ROM verification (Restart); disabled|enabled");
  init_option(&default_options[OPT_SAMPLE_RATE],         APPNAME"_sample_rate",         "Sample Rate (KHz); 48000|8000|11025|22050|44100");
  init_option(&default_options[OPT_DCS_SPEEDHACK],       APPNAME"_dcs_speedhack",       "DCS Speedhack; enabled|disabled");
  
  init_option(&default_options[OPT_end], NULL, NULL);
  
  for(default_index = 0; default_index < (OPT_end + 1); default_index++)
  {
    if((default_index == OPT_STV_BIOS && !is_stv) || (default_index == OPT_NEOGEO_BIOS && !is_neogeo))
      continue; /* only offer BIOS selection when it is relevant */
    
    effective_options[effective_index] = default_options[default_index];
    effective_index++;
  }

  environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)effective_options);
}

void init_option(struct retro_variable *option, const char *key, const char *value)
{
  option->key = key;
  option->value = value;
}

static void update_variables(bool first_time)
{
  struct retro_led_interface ledintf;
  struct retro_variable var;

  var.value = NULL;

  var.key = APPNAME"_frameskip";
  options.frameskip = 0; /* default if none set by frontend */
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    options.frameskip = atoi(var.value);
  }

  var.value = NULL;

  var.key = APPNAME"_input_interface";
  options.input_interface = RETRO_DEVICE_JOYPAD;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "retropad") == 0)
      options.input_interface = RETRO_DEVICE_JOYPAD;
    else if(strcmp(var.value, "mame_keyboard") == 0)
      options.input_interface = RETRO_DEVICE_KEYBOARD;
    else
      options.input_interface = 0; /* retropad and keyboard simultaneously. "classic mame2003 input mode" */
  }

  var.value = NULL;

  var.key = APPNAME"_retropad_layout";
  options.retropad_layout = RETROPAD_MAME;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "modern") == 0)
      options.retropad_layout = RETROPAD_MODERN;
    else if(strcmp(var.value, "SNES") == 0)
      options.retropad_layout = RETROPAD_SNES;
    else
      options.retropad_layout = RETROPAD_MAME;
    if(!first_time)
      retro_describe_buttons(); /* the first time, MAME's init_game() needs to be called before this so there is also    */
                                /* retro_describe_buttons() in retro_load_game() to take care of the initial description */
  }

  
  var.value = NULL;

  var.key = APPNAME"_mouse_device";
  options.mouse_device = 0;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "pointer") == 0)
      options.mouse_device = RETRO_DEVICE_POINTER;
    else if(strcmp(var.value, "mouse") == 0)
      options.mouse_device = RETRO_DEVICE_MOUSE;
    else
      options.mouse_device = 0;
  }

  var.value = NULL;

  var.key = APPNAME"_crosshair_enabled";
  options.crosshair_enable = 0;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "enabled") == 0)
      options.crosshair_enable = 1;
    else
      options.crosshair_enable = 0;
  }
  
  var.value = NULL;

  var.key = APPNAME"_skip_disclaimer";
  options.skip_disclaimer = false;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "enabled") == 0)
      options.skip_disclaimer = true;
    else
      options.skip_disclaimer = false;
  }

  var.value = NULL;

  var.key = APPNAME"_skip_warnings";
  options.skip_warnings = false;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "enabled") == 0)
      options.skip_warnings = true;
    else
      options.skip_warnings = false;
  }
  
  var.value = NULL;

  var.key = APPNAME"_display_setup";
  options.display_setup = 0;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "enabled") == 0)
      options.display_setup = 1;
    else
      options.display_setup = 0;
  }


  var.value = NULL;

  var.key = APPNAME"_brightness";
  options.brightness = 1.0f; /* default if none set by frontend */
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    options.brightness = atof(var.value);
    if(!first_time)
      palette_set_global_brightness(options.brightness);    
  }

  var.value = NULL;

  var.key = APPNAME"_gamma";
  options.gamma = 1.2f; /* default if none set by frontend */
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    options.gamma = atof(var.value);
    if(!first_time)
      palette_set_global_gamma(options.gamma);
  }

  /* TODO: Add overclock option. Below is the code from the old MAME osd to help process the core option.*/
  /*

  double overclock;
  int cpu, doallcpus = 0, oc;

  if (code_pressed(KEYCODE_LSHIFT) || code_pressed(KEYCODE_RSHIFT))
    doallcpus = 1;
  if (!code_pressed(KEYCODE_LCONTROL) && !code_pressed(KEYCODE_RCONTROL))
    increment *= 5;
  if( increment )
  {
    overclock = timer_get_overclock(arg);
    overclock += 0.01 * increment;
    if (overclock < 0.01) overclock = 0.01;
    if (overclock > 2.0) overclock = 2.0;
    if( doallcpus )
      for( cpu = 0; cpu < cpu_gettotalcpu(); cpu++ )
        timer_set_overclock(cpu, overclock);
    else
      timer_set_overclock(arg, overclock);
  }

  oc = 100 * timer_get_overclock(arg) + 0.5;

  if( doallcpus )
    sprintf(buf,"%s %s %3d%%", ui_getstring (UI_allcpus), ui_getstring (UI_overclock), oc);
  else
    sprintf(buf,"%s %s%d %3d%%", ui_getstring (UI_overclock), ui_getstring (UI_cpu), arg, oc);
  displayosd(bitmap,buf,oc/2,100/2);
*/

  var.value = NULL;

  var.key = APPNAME"_enable_backdrop";
  options.use_artwork = ARTWORK_USE_NONE;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "enabled") == 0)
      options.use_artwork = ARTWORK_USE_BACKDROPS;
    else
      options.use_artwork = ARTWORK_USE_NONE;
  }

  /** BEGIN BIOS OPTIONS **/

  options.bios        = NULL;
  options.neogeo_bios = NULL;
  options.stv_bios    = NULL;

  var.value = NULL;
  var.key = APPNAME"_neogeo_bios";

  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "default") != 0) /* leave as null if set to default */
      options.neogeo_bios = strdup(var.value);
  }
  
  var.value = NULL;
  var.key = APPNAME"_stv_bios";

  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "default") != 0) /* do nothing if set to default */
    {
      options.stv_bios = strdup(var.value);
    }
  }

  /** END BIOS OPTIONS **/

  var.value = NULL;

  var.key = APPNAME"_dialsharexy";
  options.dial_share_xy = 0;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "enabled") == 0)
      options.dial_share_xy = 1;
    else
      options.dial_share_xy = 0;
  }

  var.value = NULL;

  var.key = APPNAME"_dual_joysticks";
  options.dual_joysticks = false;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "enabled") == 0)
      options.dual_joysticks = true;
    else
      options.dual_joysticks = false;
  }

  if(first_time)
  {
    old_dual_joystick_state = options.dual_joysticks;
  }
  else if(old_dual_joystick_state != options.dual_joysticks)
  {
    char cfg_file_path[PATH_MAX_LENGTH];
    char buffer[PATH_MAX_LENGTH];
    osd_get_path(FILETYPE_CONFIG, buffer);
    snprintf(cfg_file_path, PATH_MAX_LENGTH, "%s%s%s.cfg", buffer, path_default_slash(), options.romset_filename_noext);
    buffer[0] = '\0';
    
    if(path_is_valid(cfg_file_path))
    {
      
      if(!remove(cfg_file_path) == 0)
      {
        snprintf(buffer, PATH_MAX_LENGTH, "%s.cfg exists but cannot be deleted!\n", options.romset_filename_noext);
      }
      else
      {
        snprintf(buffer, PATH_MAX_LENGTH, "%s.cfg exists but cannot be deleted!\n", options.romset_filename_noext);
      }
    }
    log_cb(RETRO_LOG_INFO, LOGPRE "%s Reloading input maps.\n", buffer);
    usrintf_showmessage_secs(4, "%s Reloading input maps.", buffer);
    
    load_input_port_settings();
    old_dual_joystick_state = options.dual_joysticks;  
  }
  
  var.value = NULL;

  var.key = APPNAME"_rstick_to_btns";
  options.rstick_to_btns = 0;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "enabled") == 0)
      options.rstick_to_btns = 1;
    else
      options.rstick_to_btns = 0;
  }

  var.value = NULL;

  var.key = APPNAME"_tate_mode";
  options.tate_mode = 0;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "enabled") == 0)
      options.tate_mode = 1;
    else
      options.tate_mode = 0;
  }

  var.value = NULL;

  var.key = APPNAME"_vector_resolution_multiplier";
  options.vector_resolution_multiplier = 1;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    options.vector_resolution_multiplier = atoi(var.value);
  }

  var.value = NULL;

  var.key = APPNAME"_vector_antialias";
  options.antialias = 0;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "enabled") == 0)
      options.antialias = 1; /* integer: 1 to enable antialiasing on vectors _ does not work as of 2018/04/17*/
    else
      options.antialias = 0;
  }

  var.value = NULL;

  var.key = APPNAME"_vector_translucency";
  options.translucency = 1;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "enabled") == 0)
      options.translucency = 1; /* integer: 1 to enable translucency on vectors */
    else
      options.translucency = 0;
  }

  var.value = NULL;

  var.key = APPNAME"_vector_beam_width";
  options.beam = 1;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    options.beam = atoi(var.value); /* integer: vector beam width */
  }

  var.value = NULL;

  var.key = APPNAME"_vector_flicker";
  options.vector_flicker = 20;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    options.vector_flicker = (int)(2.55 * atof(var.value)); /* why 2.55? must be an old mame family recipe */
  }

  var.value = NULL;

  var.key = APPNAME"_vector_intensity";
  options.vector_intensity_correction = 1.5f;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    options.vector_intensity_correction = atof(var.value); /* float: vector beam intensity */
  }

  var.value = NULL;

  var.key = APPNAME"_skip_rom_verify";
  options.skip_rom_verify = 0;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "enabled") == 1)
      options.skip_rom_verify = 1;
    else
      options.skip_rom_verify = 0;
  }

  var.value = NULL;

  var.key = APPNAME"_sample_rate";
  options.samplerate = 48000;

  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    options.samplerate = atoi(var.value);
  }

  var.value = NULL;

  var.key = APPNAME"_dcs_speedhack";
  options.activate_dcs_speedhack = 1;
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
  {
    if(strcmp(var.value, "enabled") == 0)
      options.activate_dcs_speedhack = 1;
    else
      options.activate_dcs_speedhack = 0;
  }

  ledintf.set_led_state = NULL;
  environ_cb(RETRO_ENVIRONMENT_GET_LED_INTERFACE, &ledintf);
  led_state_cb = ledintf.set_led_state;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   const int orientation = game_driver->flags & ORIENTATION_MASK;
   const bool rotated = ((orientation == ROT90) || (orientation == ROT270));
   
   const int width = rotated ? videoConfig.height : videoConfig.width;
   const int height = rotated ? videoConfig.width : videoConfig.height;
   
   info->geometry.base_width = width;
   info->geometry.base_height = height;
   info->geometry.max_width = width;
   info->geometry.max_height = height;
   info->geometry.aspect_ratio = (rotated && !options.tate_mode) ? (float)videoConfig.aspect_y / (float)videoConfig.aspect_x : (float)videoConfig.aspect_x / (float)videoConfig.aspect_y;
   
   if (Machine->drv->frames_per_second < 60.0 )
       info->timing.fps = 60.0; 
   else 
      info->timing.fps = Machine->drv->frames_per_second; /* qbert is 61 fps */

   info->timing.sample_rate = Machine->drv->frames_per_second * 1000;
}

unsigned retro_api_version(void)
{
  return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
  info->library_name = "MAME 2003-plus";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
  info->library_version = "0.78" GIT_VERSION;
  info->valid_extensions = "zip";
  info->need_fullpath = true;
  info->block_extract = true;
}

static const char* control_labels[RETRO_DEVICE_ID_JOYPAD_R3 + 1]; /* RETRO_DEVICE_ID_JOYPAD_R3 is the retropad input with the highest index # */


const char *sf2_btn_label(int input)
{
  switch(input)
  {
    case IPT_BUTTON1:
      return BTN1 "Weak Kick";
    case IPT_BUTTON2:
      return BTN2 "Medium Kick";
    case IPT_BUTTON3:
      return BTN3 "Strong Punch";
    case IPT_BUTTON4:
      return BTN4 "Weak Punch";
    case IPT_BUTTON5:
      return BTN5 "Medium Punch";
    case IPT_BUTTON6:
      return BTN6 "Strong Kick";
  } 
  return NULL;
}

const char *neogeo_btn_label(int input)
{
  switch(input)
  {
    case IPT_BUTTON1:
      return BTN1 "Neo Geo A";
    case IPT_BUTTON2:
      return BTN2 "Neo Geo B";
    case IPT_BUTTON3:
      return BTN3 "Neo Geo C";
    case IPT_BUTTON4:
      return BTN4 "Neo Geo D";
  }
  return NULL;
}

const char *generic_btn_label(int input)
{
  unsigned int i = 0;

  for( ; default_inputs[i].type != IPT_END; ++i)
  {
    struct ipd *entry = &default_inputs[i];
    if(entry->type == input)
    {
      if(input >= IPT_SERVICE1 && input <= IPT_UI_EDIT_CHEAT)
        return entry->name; /* these strings are not player-specific */
      else /* start with the third character, trimming the initial 'P1', 'P2', etc which we don't need for libretro */
        return &entry->name[3];
    }
  }
  return NULL;
}

const char *null_btn_labeler(int input)
{
  return NULL;
}
 
const char *button_labeler(int input)
{
  if(!string_is_empty((*content_aware_labeler)(input)))
    return (*content_aware_labeler)(input);
  
  return generic_btn_label(input);
}

void retro_describe_buttons(void)
{
  /* Assuming the standard RetroPad layout:
   *
   *   [L2]                                 [R2]
   *   [L]                                   [R]
   *
   *     [^]                               [X]
   *
   * [<]     [>]    [start] [selct]    [Y]     [A]
   *
   *     [v]                               [B]
   *
   *
   * or standard RetroPad fight stick layout:
   *
   *   [start] [selct]
   *                                         [X]  [R]  [L]
   *     [^]                            [Y]
   *
   * [<]     [>]                             [A]  [R2] [L2]
   *                                    [B]
   *     [v]
   *
   *
   *
   * key: [MAME button/Street Fighter II move]
   *
   * options.retropad_layout == RETROPAD_MODERN
   * ========================
   * Uses the fight stick & pad layout popularised by Street Figher IV.
   * Needs an 8+ button controller by default.
   *
   * [8/-]                                     [6/HK]  |
   * [7/-]                                     [3/HP]  |
   *                                                   |        [2/MP]  [3/HP]  [7/-]
   *     [^]                               [2/MP]      |  [1/LP]
   *                                                   |
   * [<]     [>]    [start] [selct]    [1/LP]  [5/MK]  |        [5/MK]  [6/HK]  [8/-]
   *                                                   |  [4/LK]
   *     [v]                               [4/LK]      |
   *                                                   |
   *
   * retropad_layout == RETROPAD_SNES
   * ========================
   * Uses the layout popularised by SNES Street Figher II.
   * Only needs a 6+ button controller by default, doesn't suit 8+ button fight sticks.
   *
   * [7/-]                                      [8/-]  |
   * [3/HP]                                    [6/HK]  |
   *                                                   |        [2/MP]  [6/HK]  [3/HP]
   *     [^]                               [2/MP]      |  [1/LP]
   *                                                   |
   * [<]     [>]    [start] [selct]    [1/LP]  [5/MK]  |        [5/MK]  [8/-]   [7/-]
   *                                                   |  [4/LK]
   *     [v]                               [4/LK]      |
   *                                                   |
   *
   * options.retropad_layout == RETROPAD_MAME
   * ========================
   * Uses current MAME's default Xbox 360 controller layout.
   * Not sensible for 6 button fighters, but may suit other games.
   *
   * [7/-]                                     [8/-]   |
   * [5/MK]                                    [6/HK]  |
   *                                                   |        [4/WK]  [6/HK]  [5/MK]
   *     [^]                               [4/WK]      |  [3/HP]
   *                                                   |
   * [<]     [>]    [start] [selct]    [3/HP]  [2/MP]  |        [2/MP]  [8/-]   [7/-]
   *                                                   |  [1/LP]
   *     [v]                               [1/LP]      |
   *                                                   |
   */

   
  /************  BASELINE "CLASSIC" MAPPING  ************/   
  
  log_cb(RETRO_LOG_ERROR, "\nhere 1\n");
  control_labels[RETRO_DEVICE_ID_JOYPAD_LEFT]   = button_labeler(IPT_JOYSTICK_LEFT);
  control_labels[RETRO_DEVICE_ID_JOYPAD_RIGHT]  = button_labeler(IPT_JOYSTICK_RIGHT);
  control_labels[RETRO_DEVICE_ID_JOYPAD_UP]     = button_labeler(IPT_JOYSTICK_UP);
  control_labels[RETRO_DEVICE_ID_JOYPAD_DOWN]   = button_labeler(IPT_JOYSTICK_DOWN);
  control_labels[RETRO_DEVICE_ID_JOYPAD_B]      = button_labeler(IPT_BUTTON1);
  control_labels[RETRO_DEVICE_ID_JOYPAD_Y]      = button_labeler(IPT_BUTTON3);
  control_labels[RETRO_DEVICE_ID_JOYPAD_X]      = button_labeler(IPT_BUTTON4);
  control_labels[RETRO_DEVICE_ID_JOYPAD_A]      = button_labeler(IPT_BUTTON2);
  control_labels[RETRO_DEVICE_ID_JOYPAD_L]      = button_labeler(IPT_BUTTON5);
  control_labels[RETRO_DEVICE_ID_JOYPAD_R]      = button_labeler(IPT_BUTTON6);
  control_labels[RETRO_DEVICE_ID_JOYPAD_L2]     = button_labeler(IPT_BUTTON7);
  control_labels[RETRO_DEVICE_ID_JOYPAD_R2]     = button_labeler(IPT_BUTTON8);
  control_labels[RETRO_DEVICE_ID_JOYPAD_L3]     = button_labeler(IPT_BUTTON9);
  control_labels[RETRO_DEVICE_ID_JOYPAD_R3]     = button_labeler(IPT_BUTTON10);
  control_labels[RETRO_DEVICE_ID_JOYPAD_SELECT] = button_labeler(IPT_COIN1);  /* MAME uses distinct strings for each player input, while */
  control_labels[RETRO_DEVICE_ID_JOYPAD_START]  = button_labeler(IPT_START1); /* libretro uses a unified strings across players and adds their */
                                                                              /* index # dynamically. We grab the IPT_START1 & IPT_COIN1 */
                                                                              /* labels from MAME and then make them generic for libretro. */
  

    log_cb(RETRO_LOG_ERROR, "\nhere 2\n");

  /************  "MODERN" MAPPING OVERLAY  ************/ 
  if(options.retropad_layout == RETROPAD_MODERN)
  {
    control_labels[RETRO_DEVICE_ID_JOYPAD_B]     = button_labeler(IPT_BUTTON4);
    control_labels[RETRO_DEVICE_ID_JOYPAD_Y]     = button_labeler(IPT_BUTTON1);
    control_labels[RETRO_DEVICE_ID_JOYPAD_X]     = button_labeler(IPT_BUTTON2);
    control_labels[RETRO_DEVICE_ID_JOYPAD_A]     = button_labeler(IPT_BUTTON5);
    control_labels[RETRO_DEVICE_ID_JOYPAD_L]     = button_labeler(IPT_BUTTON7);
    control_labels[RETRO_DEVICE_ID_JOYPAD_R]     = button_labeler(IPT_BUTTON3);
    control_labels[RETRO_DEVICE_ID_JOYPAD_L2]    = button_labeler(IPT_BUTTON8);
    control_labels[RETRO_DEVICE_ID_JOYPAD_R2]    = button_labeler(IPT_BUTTON6);
  }
  
  /************  "MODERN" MAPPING OVERLAY  ************/
  if(options.retropad_layout == RETROPAD_SNES)
  {
    control_labels[RETRO_DEVICE_ID_JOYPAD_B]    = button_labeler(IPT_BUTTON4);
    control_labels[RETRO_DEVICE_ID_JOYPAD_Y]     = button_labeler(IPT_BUTTON1);
    control_labels[RETRO_DEVICE_ID_JOYPAD_X]    = button_labeler(IPT_BUTTON2);
    control_labels[RETRO_DEVICE_ID_JOYPAD_A]    = button_labeler(IPT_BUTTON5);
    control_labels[RETRO_DEVICE_ID_JOYPAD_L]    = button_labeler(IPT_BUTTON3);
  }
   log_cb(RETRO_LOG_ERROR, "\nhere 3\n");
 

#define describe_buttons(INDEX) \
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   control_labels[RETRO_DEVICE_ID_JOYPAD_LEFT]   },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  control_labels[RETRO_DEVICE_ID_JOYPAD_RIGHT]  },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     control_labels[RETRO_DEVICE_ID_JOYPAD_UP]     },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   control_labels[RETRO_DEVICE_ID_JOYPAD_DOWN]   },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      control_labels[RETRO_DEVICE_ID_JOYPAD_B]      },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      control_labels[RETRO_DEVICE_ID_JOYPAD_Y]      },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      control_labels[RETRO_DEVICE_ID_JOYPAD_X]      },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      control_labels[RETRO_DEVICE_ID_JOYPAD_A]      },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      control_labels[RETRO_DEVICE_ID_JOYPAD_L]      },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      control_labels[RETRO_DEVICE_ID_JOYPAD_R]      },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     control_labels[RETRO_DEVICE_ID_JOYPAD_L2]     },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     control_labels[RETRO_DEVICE_ID_JOYPAD_R2]     },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     control_labels[RETRO_DEVICE_ID_JOYPAD_L3]     },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     control_labels[RETRO_DEVICE_ID_JOYPAD_R3]     },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, control_labels[RETRO_DEVICE_ID_JOYPAD_SELECT] },\
{ INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  control_labels[RETRO_DEVICE_ID_JOYPAD_START]  },
  
  struct retro_input_descriptor desc[] = {
    describe_buttons(0)
    describe_buttons(1)
    describe_buttons(2)
    describe_buttons(3)
    { 0, 0, 0, 0, NULL }
  };
  
  environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}


bool retro_load_game(const struct retro_game_info *game)
{
  int              driverIndex    = 0;
  char             *driver_lookup = NULL;
  int              orientation    = 0;
  unsigned         rotateMode     = 0;
  static const int uiModes[]      = {ROT0, ROT90, ROT180, ROT270};

  if(string_is_empty(game->path))
  {
    log_cb(RETRO_LOG_ERROR, LOGPRE "Content path is not set. Exiting!\n");
    return false;
  }

  log_cb(RETRO_LOG_INFO, LOGPRE "Content path: %s.\n", game->path);    
  if(!path_is_valid(game->path))
  {
    log_cb(RETRO_LOG_ERROR, LOGPRE "Content path is not valid. Exiting!");
    return false;
  }

  driver_lookup = strdup(path_basename(game->path));
  path_remove_extension(driver_lookup);

  log_cb(RETRO_LOG_INFO, LOGPRE "Content lookup name: %s.\n", driver_lookup);

  for (driverIndex = 0; driverIndex < total_drivers; driverIndex++)
  {
    const struct GameDriver *needle = drivers[driverIndex];

    if ((strcasecmp(driver_lookup, needle->description) == 0) 
      || (strcasecmp(driver_lookup, needle->name) == 0) )
    {
      log_cb(RETRO_LOG_INFO, LOGPRE "Total MAME drivers: %i. Matched game driver: %s.\n", (int) total_drivers, needle->name);
      game_driver = needle;
      options.romset_filename_noext = driver_lookup;
      
      if( (strcmp(game_driver->name, "sf2") == 0)
      ||  (game_driver->clone_of && !string_is_empty(game_driver->clone_of->name) && (strcmp(game_driver->clone_of->name, "sf2") == 0)) )
      {
        is_sf2_layout = true; /* are there other games with this layout? */
        content_aware_labeler = &sf2_btn_label;
        log_cb(RETRO_LOG_INFO, LOGPRE "Content identified as Street Fighter 2 or a clone.\n");
      }

      if(game_driver->bios != NULL) /* this is a driver that uses a bios, but which bios is it? */
      {
        const char *ancestor_name = NULL;
        if(game_driver->clone_of && !string_is_empty(game_driver->clone_of->name))
        {
          ancestor_name = game_driver->clone_of->name;

          if(game_driver->clone_of->clone_of && !string_is_empty(game_driver->clone_of->clone_of->name)) /* search up to two levels up */
            ancestor_name = game_driver->clone_of->clone_of->name;                                       /* that's enough, right? */
          else
            ancestor_name = game_driver->clone_of->name;
        }
        
        if(!string_is_empty(ancestor_name))
        {
          if(strcmp(ancestor_name, "neogeo") == 0)
          {
            is_neogeo = true;
            content_aware_labeler = &neogeo_btn_label;
            log_cb(RETRO_LOG_INFO, LOGPRE "Content identified as a Neo Geo game.\n");
          }
          else if(strcmp(ancestor_name, "stvbios") == 0)
          {
            is_stv = true;
            log_cb(RETRO_LOG_INFO, LOGPRE "Content identified as a ST-V game.\n");
          }
        }
      }
      break;
    }
  }

  if(driverIndex == total_drivers)
  {
      log_cb(RETRO_LOG_ERROR, LOGPRE "Total MAME drivers: %i. MAME driver not found for selected game!", (int) total_drivers);
      return false;
  }

  options.libretro_content_path = strdup(game->path);
  path_basedir(options.libretro_content_path);

  /* Get system directory from frontend */
  options.libretro_system_path = NULL;
  environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,&options.libretro_system_path);
  if (options.libretro_system_path == NULL || options.libretro_system_path[0] == '\0')
  {
      log_cb(RETRO_LOG_INFO, LOGPRE "libretro system path not set by frontend, using content path\n");
      options.libretro_system_path = options.libretro_content_path;
  }
  
  /* Get save directory from frontend */
  options.libretro_save_path = NULL;
  environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY,&options.libretro_save_path);
  if (options.libretro_save_path == NULL || options.libretro_save_path[0] == '\0')
  {
      log_cb(RETRO_LOG_INFO,  LOGPRE "libretro save path not set by frontent, using content path\n");
      options.libretro_save_path = options.libretro_content_path;
  }

  log_cb(RETRO_LOG_INFO, LOGPRE "content path: %s\n", options.libretro_content_path);
  log_cb(RETRO_LOG_INFO, LOGPRE " system path: %s\n", options.libretro_system_path);
  log_cb(RETRO_LOG_INFO, LOGPRE "   save path: %s\n", options.libretro_save_path);

  /* Setup Rotation */
  rotateMode = 0;        
  orientation = drivers[driverIndex]->flags & ORIENTATION_MASK;
  
  rotateMode = (orientation == ROT270) ? 1 : rotateMode;
  rotateMode = (orientation == ROT180) ? 2 : rotateMode;
  rotateMode = (orientation == ROT90) ? 3 : rotateMode;
  
  environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rotateMode);
  options.ui_orientation = uiModes[rotateMode];

  init_core_options();
  update_variables(true);

  if(is_neogeo)
    options.bios = options.neogeo_bios;
  else if(is_stv)
    options.bios = options.stv_bios;

  if(!init_game(driverIndex))
    return false;
  
  retro_describe_buttons(); /* needs to be called after init_game() in order to use MAME button label strings */

  if(!run_game(driverIndex))
    return false;
  
  return true;
}

void retro_reset (void)
{
    machine_reset(); /* use MAME function */
}

/* get pointer axis vector from coord */
int16_t get_pointer_delta(int16_t coord, int16_t *prev_coord)
{
   int16_t delta = 0;
   if (*prev_coord == 0 || coord == 0)
   {
      *prev_coord = coord;
   }
   else
   {
      if (coord != *prev_coord)
      {
         delta = coord - *prev_coord;
         *prev_coord = coord;
      }
   }
   
   return delta;
}

void retro_run (void)
{
   int i;
   bool pointer_pressed;
   const struct KeyboardInfo *thisInput;
   bool updated = false;

   poll_cb();

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables(false);

   /* Keyboard */
   thisInput = retroKeys;
   while(thisInput->name)
   {
      retroKeyState[thisInput->code] = input_cb(0, RETRO_DEVICE_KEYBOARD, 0, thisInput->code);
      thisInput ++;
   }
   
   for (i = 0; i < 4; i ++)
   {
      unsigned int offset = (i * 18);

      /* Analog joystick */
      analogjoy[i][0] = input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
      analogjoy[i][1] = input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
      analogjoy[i][2] = input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
      analogjoy[i][3] = input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
      
      /* Joystick */
      if (options.rstick_to_btns)
      {
         /* if less than 0.5 force, ignore and read buttons as usual */
         retroJsState[0 + offset] = analogjoy[i][3] >  0x4000 ? 1 : input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
         retroJsState[1 + offset] = analogjoy[i][2] < -0x4000 ? 1 : input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);
      }
      else
      {
         retroJsState[0 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
         retroJsState[1 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);
      }
      retroJsState[2 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);
      retroJsState[3 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
      retroJsState[4 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
      retroJsState[5 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
      retroJsState[6 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
      retroJsState[7 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      if (options.rstick_to_btns)
      {
         retroJsState[8 + offset] = analogjoy[i][2] >  0x4000 ? 1 : input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
         retroJsState[9 + offset] = analogjoy[i][3] < -0x4000 ? 1 : input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
      }
      else
      {
         retroJsState[8 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
         retroJsState[9 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
      }
      retroJsState[10 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
      retroJsState[11 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
      retroJsState[12 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2);
      retroJsState[13 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);
      retroJsState[14 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3);
      retroJsState[15 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3);
      
      if (options.mouse_device)
      {
         if (options.mouse_device == RETRO_DEVICE_MOUSE)
         {
            retroJsState[16 + offset] = input_cb(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
            retroJsState[17 + offset] = input_cb(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);
            mouse_x[i] = input_cb(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
            mouse_y[i] = input_cb(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
         }
         else /* RETRO_DEVICE_POINTER */
         {
            pointer_pressed = input_cb(i, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
            retroJsState[16 + offset] = pointer_pressed;
            retroJsState[17 + offset] = 0; /* padding */
            mouse_x[i] = pointer_pressed ? get_pointer_delta(input_cb(i, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X), &prev_pointer_x) : 0;
            mouse_y[i] = pointer_pressed ? get_pointer_delta(input_cb(i, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y), &prev_pointer_y) : 0;
         }
      }
      else
      {
         retroJsState[16 + offset] = 0;
         retroJsState[17 + offset] = 0;
      }
   }

   mame_frame();

}

void retro_unload_game(void)
{
    mame_done();
    /* do we need to be freeing things here? */
    
    free(options.romset_filename_noext);
    if(options.bios)        free(options.bios);
    if(options.neogeo_bios) free(options.neogeo_bios);
    if(options.stv_bios)    free(options.stv_bios);   
}

void retro_deinit(void)
{
#ifdef LOG_PERFORMANCE
   perf_cb.perf_log();
#endif
}


size_t retro_serialize_size(void)
{
    extern size_t state_get_dump_size(void);
    
    return state_get_dump_size();
}

bool retro_serialize(void *data, size_t size)
{
   int cpunum;
	if(retro_serialize_size() && data && size)
	{
		/* write the save state */
		state_save_save_begin(data);

		/* write tag 0 */
		state_save_set_current_tag(0);
		if(state_save_save_continue())
		{
		    return false;
		}

		/* loop over CPUs */
		for (cpunum = 0; cpunum < cpu_gettotalcpu(); cpunum++)
		{
			cpuintrf_push_context(cpunum);

			/* make sure banking is set */
			activecpu_reset_banking();

			/* save the CPU data */
			state_save_set_current_tag(cpunum + 1);
			if(state_save_save_continue())
			    return false;

			cpuintrf_pop_context();
		}

		/* finish and close */
		state_save_save_finish();
		
		return true;
	}

	return false;
}

bool retro_unserialize(const void * data, size_t size)
{
    int cpunum;
	/* if successful, load it */
	if (retro_serialize_size() && data && size && !state_save_load_begin((void*)data, size))
	{
        /* read tag 0 */
        state_save_set_current_tag(0);
        if(state_save_load_continue())
            return false;

        /* loop over CPUs */
        for (cpunum = 0; cpunum < cpu_gettotalcpu(); cpunum++)
        {
            cpuintrf_push_context(cpunum);

            /* make sure banking is set */
            activecpu_reset_banking();

            /* load the CPU data */
            state_save_set_current_tag(cpunum + 1);
            if(state_save_load_continue())
                return false;

            cpuintrf_pop_context();
        }

        /* finish and close */
        state_save_load_finish();

        
        return true;
	}

	return false;
}

/******************************************************************************

  Sound

  osd_start_audio_stream() is called at the start of the emulation to initialize
  the output stream, then osd_update_audio_stream() is called every frame to
  feed new data. osd_stop_audio_stream() is called when the emulation is stopped.

  The sample rate is fixed at Machine->sample_rate. Samples are 16-bit, signed.
  When the stream is stereo, left and right samples are alternated in the
  stream.

  osd_start_audio_stream() and osd_update_audio_stream() must return the number
  of samples (or couples of samples, when using stereo) required for next frame.
  This will be around Machine->sample_rate / Machine->drv->frames_per_second,
  the code may adjust it by SMALL AMOUNTS to keep timing accurate and to
  maintain audio and video in sync when using vsync. Note that sound emulation,
  especially when DACs are involved, greatly depends on the number of samples
  per frame to be roughly constant, so the returned value must always stay close
  to the reference value of Machine->sample_rate / Machine->drv->frames_per_second.
  Of course that value is not necessarily an integer so at least a +/- 1
  adjustment is necessary to avoid drifting over time.

******************************************************************************/

int osd_start_audio_stream(int stereo)
{
	 Machine->sample_rate = Machine->drv->frames_per_second * 1000;
	delta_samples = 0.0f;
	usestereo = stereo ? 1 : 0;

	/* determine the number of samples per frame */
	samples_per_frame = Machine->sample_rate / Machine->drv->frames_per_second;

	if (Machine->sample_rate == 0) return 0;

	samples_buffer = (short *) calloc(samples_per_frame, 2 + usestereo * 2);
	if (!usestereo) conversion_buffer = (short *) calloc(samples_per_frame, 4);
	
	return samples_per_frame;
}


int osd_update_audio_stream(INT16 *buffer)
{
	int i,j;

	if ( Machine->sample_rate !=0 && buffer )
	{
   		memcpy(samples_buffer, buffer, samples_per_frame * (usestereo ? 4 : 2));
		if (usestereo)
			audio_batch_cb(samples_buffer, samples_per_frame);
		else
		{
			for (i = 0, j = 0; i < samples_per_frame; i++)
        		{
				conversion_buffer[j++] = samples_buffer[i];
				conversion_buffer[j++] = samples_buffer[i];
		        }
         		audio_batch_cb(conversion_buffer,samples_per_frame);
		}	
   		delta_samples += (Machine->sample_rate / Machine->drv->frames_per_second) - samples_per_frame;
		if (delta_samples >= 1.0f)
		{
			int integer_delta = (int)delta_samples;
			samples_per_frame += integer_delta;
			delta_samples -= integer_delta;
		}
	}
	return samples_per_frame;
}


void osd_stop_audio_stream(void)
{
}

/******************************************************************************

Miscellaneous

******************************************************************************/

unsigned retro_get_region (void) {return RETRO_REGION_NTSC;}
void *retro_get_memory_data(unsigned type) {return 0;}
size_t retro_get_memory_size(unsigned type) {return 0;}
bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info){return false;}
void retro_cheat_reset(void){}
void retro_cheat_set(unsigned unused, bool unused1, const char* unused2){}
void retro_set_controller_port_device(unsigned in_port, unsigned device){}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }


/******************************************************************************

	Keymapping

******************************************************************************/

#define EMIT_RETRO_PAD_DIRECTIONS(INDEX) \
  {"RetroPad"   #INDEX " Left",        ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_LEFT,   JOYCODE_##INDEX##_LEFT}, \
  {"RetroPad"   #INDEX " Right",       ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_RIGHT,  JOYCODE_##INDEX##_RIGHT}, \
  {"RetroPad"   #INDEX " Up",          ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_UP,     JOYCODE_##INDEX##_UP}, \
  {"RetroPad"   #INDEX " Down",        ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_DOWN,   JOYCODE_##INDEX##_DOWN}, \
  {"RetroPad"   #INDEX " B",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_B,      JOYCODE_##INDEX##_RIGHT_DOWN}, \
  {"RetroPad"   #INDEX " Y",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_Y,      JOYCODE_##INDEX##_RIGHT_LEFT}, \
  {"RetroPad"   #INDEX " X",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_X,      JOYCODE_##INDEX##_RIGHT_UP}, \
  {"RetroPad"   #INDEX " A",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_A,      JOYCODE_##INDEX##_RIGHT_RIGHT}

#define EMIT_RETRO_PAD(INDEX) \
  {"RetroPad"   #INDEX " B",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_B,      JOYCODE_##INDEX##_BUTTON1}, \
  {"RetroPad"   #INDEX " Y",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_Y,      JOYCODE_##INDEX##_BUTTON3}, \
  {"RetroPad"   #INDEX " X",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_X,      JOYCODE_##INDEX##_BUTTON4}, \
  {"RetroPad"   #INDEX " A",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_A,      JOYCODE_##INDEX##_BUTTON2}, \
  {"RetroPad"   #INDEX " L",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_L,      JOYCODE_##INDEX##_BUTTON5}, \
  {"RetroPad"   #INDEX " R",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_R,      JOYCODE_##INDEX##_BUTTON6}, \
  {"RetroPad"   #INDEX " L2",          ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_L2,     JOYCODE_##INDEX##_BUTTON7}, \
  {"RetroPad"   #INDEX " R2",          ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_R2,     JOYCODE_##INDEX##_BUTTON8}, \
  {"RetroPad"   #INDEX " L3",          ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_L3,     JOYCODE_##INDEX##_BUTTON9}, \
  {"RetroPad"   #INDEX " R3",          ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_R3,     JOYCODE_##INDEX##_BUTTON10}, \
  {"RetroPad"   #INDEX " Start",       ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_START,  JOYCODE_##INDEX##_START}, \
  {"RetroPad"   #INDEX " Select",      ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_SELECT, JOYCODE_##INDEX##_SELECT}

#define EMIT_RETRO_PAD_MODERN(INDEX) \
  {"RetroPad"   #INDEX " B",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_B,      JOYCODE_##INDEX##_BUTTON4}, \
  {"RetroPad"   #INDEX " Y",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_Y,      JOYCODE_##INDEX##_BUTTON1}, \
  {"RetroPad"   #INDEX " X",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_X,      JOYCODE_##INDEX##_BUTTON2}, \
  {"RetroPad"   #INDEX " A",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_A,      JOYCODE_##INDEX##_BUTTON5}, \
  {"RetroPad"   #INDEX " L",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_L,      JOYCODE_##INDEX##_BUTTON7}, \
  {"RetroPad"   #INDEX " R",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_R,      JOYCODE_##INDEX##_BUTTON3}, \
  {"RetroPad"   #INDEX " L2",          ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_L2,     JOYCODE_##INDEX##_BUTTON8}, \
  {"RetroPad"   #INDEX " R2",          ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_R2,     JOYCODE_##INDEX##_BUTTON6}, \
  {"RetroPad"   #INDEX " L3",          ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_L3,     JOYCODE_##INDEX##_BUTTON9}, \
  {"RetroPad"   #INDEX " R3",          ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_R3,     JOYCODE_##INDEX##_BUTTON10}, \
  {"RetroPad"   #INDEX " Start",       ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_START,  JOYCODE_##INDEX##_START}, \
  {"RetroPad"   #INDEX " Select",      ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_SELECT, JOYCODE_##INDEX##_SELECT}

#define EMIT_RETRO_PAD_SNES(INDEX) \
  {"RetroPad"   #INDEX " B",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_B,      JOYCODE_##INDEX##_BUTTON4}, \
  {"RetroPad"   #INDEX " Y",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_Y,      JOYCODE_##INDEX##_BUTTON1}, \
  {"RetroPad"   #INDEX " X",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_X,      JOYCODE_##INDEX##_BUTTON2}, \
  {"RetroPad"   #INDEX " A",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_A,      JOYCODE_##INDEX##_BUTTON5}, \
  {"RetroPad"   #INDEX " L",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_L,      JOYCODE_##INDEX##_BUTTON3}, \
  {"RetroPad"   #INDEX " R",           ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_R,      JOYCODE_##INDEX##_BUTTON6}, \
  {"RetroPad"   #INDEX " L2",          ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_L2,     JOYCODE_##INDEX##_BUTTON7}, \
  {"RetroPad"   #INDEX " R2",          ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_R2,     JOYCODE_##INDEX##_BUTTON8}, \
  {"RetroPad"   #INDEX " L3",          ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_L3,     JOYCODE_##INDEX##_BUTTON9}, \
  {"RetroPad"   #INDEX " R3",          ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_R3,     JOYCODE_##INDEX##_BUTTON10}, \
  {"RetroPad"   #INDEX " Start",       ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_START,  JOYCODE_##INDEX##_START}, \
  {"RetroPad"   #INDEX " Select",      ((INDEX - 1) * 18) + RETRO_DEVICE_ID_JOYPAD_SELECT, JOYCODE_##INDEX##_SELECT}

#define EMIT_RETRO_PAD_MOUSE(INDEX) \
  {"RetroMouse" #INDEX " Left Click",  ((INDEX - 1) * 18) + 16,                            JOYCODE_MOUSE_##INDEX##_BUTTON1}, \
  {"RetroMouse" #INDEX " Right Click", ((INDEX - 1) * 18) + 17,                            JOYCODE_MOUSE_##INDEX##_BUTTON2}

struct JoystickInfo jsItems[] =
{
  EMIT_RETRO_PAD_DIRECTIONS(1),EMIT_RETRO_PAD(1),EMIT_RETRO_PAD_MOUSE(1),
  EMIT_RETRO_PAD_DIRECTIONS(2),EMIT_RETRO_PAD(2),EMIT_RETRO_PAD_MOUSE(2),
  EMIT_RETRO_PAD_DIRECTIONS(3),EMIT_RETRO_PAD(3),EMIT_RETRO_PAD_MOUSE(3),
  EMIT_RETRO_PAD_DIRECTIONS(4),EMIT_RETRO_PAD(4),EMIT_RETRO_PAD_MOUSE(4),
  {0, 0, 0}
};

struct JoystickInfo jsItemsModern[] =
{
  EMIT_RETRO_PAD_DIRECTIONS(1),EMIT_RETRO_PAD_MODERN(1),EMIT_RETRO_PAD_MOUSE(1),
  EMIT_RETRO_PAD_DIRECTIONS(2),EMIT_RETRO_PAD_MODERN(2),EMIT_RETRO_PAD_MOUSE(2),
  EMIT_RETRO_PAD_DIRECTIONS(3),EMIT_RETRO_PAD_MODERN(3),EMIT_RETRO_PAD_MOUSE(3),
  EMIT_RETRO_PAD_DIRECTIONS(4),EMIT_RETRO_PAD_MODERN(4),EMIT_RETRO_PAD_MOUSE(4),
  {0, 0, 0}
};

struct JoystickInfo jsItemsSNES[] =
{
  EMIT_RETRO_PAD_DIRECTIONS(1),EMIT_RETRO_PAD_SNES(1),EMIT_RETRO_PAD_MOUSE(1),
  EMIT_RETRO_PAD_DIRECTIONS(2),EMIT_RETRO_PAD_SNES(2),EMIT_RETRO_PAD_MOUSE(2),
  EMIT_RETRO_PAD_DIRECTIONS(3),EMIT_RETRO_PAD_SNES(3),EMIT_RETRO_PAD_MOUSE(3),
  EMIT_RETRO_PAD_DIRECTIONS(4),EMIT_RETRO_PAD_SNES(4),EMIT_RETRO_PAD_MOUSE(4),
  {0, 0, 0}
};

/******************************************************************************

	Joystick & Mouse/Trackball

******************************************************************************/

int retroJsState[72];
int16_t mouse_x[4];
int16_t mouse_y[4];
int16_t analogjoy[4][4];

const struct JoystickInfo *osd_get_joy_list(void)
{
  if(options.retropad_layout == RETROPAD_MODERN)
    return jsItemsModern;
  else if(options.retropad_layout == RETROPAD_SNES)
    return jsItemsSNES;
  else
    return jsItems;
}

int osd_is_joy_pressed(int joycode)
{
  if(options.input_interface == RETRO_DEVICE_KEYBOARD)
    return 0;
  return (joycode >= 0) ? retroJsState[joycode] : 0;
}

int osd_is_joystick_axis_code(int joycode)
{
    return 0;
}

void osd_lightgun_read(int player, int *deltax, int *deltay)
{

}

void osd_trak_read(int player, int *deltax, int *deltay)
{
    *deltax = mouse_x[player];
    *deltay = mouse_y[player];
}

int convert_analog_scale(int input)
{
    static int libretro_analog_range = LIBRETRO_ANALOG_MAX - LIBRETRO_ANALOG_MIN;
    static int analog_range = ANALOG_MAX - ANALOG_MIN;

    return (input - LIBRETRO_ANALOG_MIN)*analog_range / libretro_analog_range + ANALOG_MIN;
}

void osd_analogjoy_read(int player,int analog_axis[MAX_ANALOG_AXES], InputCode analogjoy_input[MAX_ANALOG_AXES])
{
    int i;
    for (i = 0; i < MAX_ANALOG_AXES; i ++)
    {
        if (analogjoy[player][i])
            analog_axis[i] = convert_analog_scale(analogjoy[player][i]);
    }

    analogjoy_input[0] = IPT_AD_STICK_X;
    analogjoy_input[1] = IPT_AD_STICK_Y;
}

void osd_customize_inputport_defaults(struct ipd *defaults)
{
  unsigned int i = 0;
  default_inputs = defaults;

  for( ; default_inputs[i].type != IPT_END; ++i)
  {
    struct ipd *entry = &default_inputs[i];

    if(options.dual_joysticks)
    {
      switch(entry->type)
      {
         case (IPT_JOYSTICKRIGHT_UP   | IPF_PLAYER1):
            seq_set_1(entry->seq, JOYCODE_2_UP);
            break;
         case (IPT_JOYSTICKRIGHT_DOWN | IPF_PLAYER1):
            seq_set_1(entry->seq, JOYCODE_2_DOWN);
            break;
         case (IPT_JOYSTICKRIGHT_LEFT | IPF_PLAYER1):
            seq_set_1(entry->seq, JOYCODE_2_LEFT);
            break;
         case (IPT_JOYSTICKRIGHT_RIGHT | IPF_PLAYER1):
            seq_set_1(entry->seq, JOYCODE_2_RIGHT);
            break;
         case (IPT_JOYSTICK_UP   | IPF_PLAYER2):
            seq_set_1(entry->seq, JOYCODE_3_UP);
            break;
         case (IPT_JOYSTICK_DOWN | IPF_PLAYER2):
            seq_set_1(entry->seq, JOYCODE_3_DOWN);
            break;
         case (IPT_JOYSTICK_LEFT | IPF_PLAYER2):
            seq_set_1(entry->seq, JOYCODE_3_LEFT);
            break;
         case (IPT_JOYSTICK_RIGHT | IPF_PLAYER2):
            seq_set_1(entry->seq, JOYCODE_3_RIGHT);
            break; 
         case (IPT_JOYSTICKRIGHT_UP   | IPF_PLAYER2):
            seq_set_1(entry->seq, JOYCODE_4_UP);
            break;
         case (IPT_JOYSTICKRIGHT_DOWN | IPF_PLAYER2):
            seq_set_1(entry->seq, JOYCODE_4_DOWN);
            break;
         case (IPT_JOYSTICKRIGHT_LEFT | IPF_PLAYER2):
            seq_set_1(entry->seq, JOYCODE_4_LEFT);
            break;
         case (IPT_JOYSTICKRIGHT_RIGHT | IPF_PLAYER2):
            seq_set_1(entry->seq, JOYCODE_4_RIGHT);
            break;
         case (IPT_JOYSTICKLEFT_UP   | IPF_PLAYER2):
            seq_set_1(entry->seq, JOYCODE_3_UP);
            break;
         case (IPT_JOYSTICKLEFT_DOWN | IPF_PLAYER2):
            seq_set_1(entry->seq, JOYCODE_3_DOWN);
            break;
         case (IPT_JOYSTICKLEFT_LEFT | IPF_PLAYER2):
            seq_set_1(entry->seq, JOYCODE_3_LEFT);
            break;
         case (IPT_JOYSTICKLEFT_RIGHT | IPF_PLAYER2):
            seq_set_1(entry->seq, JOYCODE_3_RIGHT);
            break;
     }
    }
   }
}

/* These calibration functions should never actually be used (as long as needs_calibration returns 0 anyway).*/
int osd_joystick_needs_calibration(void) { return 0; }
void osd_joystick_start_calibration(void){ }
const char *osd_joystick_calibrate_next(void) { return 0; }
void osd_joystick_calibrate(void) { }
void osd_joystick_end_calibration(void) { }


/******************************************************************************

	Keyboard

******************************************************************************/

extern const struct KeyboardInfo retroKeys[];
int retroKeyState[512];

const struct KeyboardInfo *osd_get_key_list(void)
{
  return retroKeys;
}

int osd_is_key_pressed(int keycode)
{
  if(options.input_interface == RETRO_DEVICE_JOYPAD)
    return 0;
  return (keycode < 512 && keycode >= 0) ? retroKeyState[keycode] : 0;
}


int osd_readkey_unicode(int flush)
{
  /* TODO*/
  return 0;
}

/******************************************************************************

	Keymapping

******************************************************************************/

/* Unassigned keycodes*/
/*	KEYCODE_OPENBRACE, KEYCODE_CLOSEBRACE, KEYCODE_BACKSLASH2, KEYCODE_STOP, KEYCODE_LWIN, KEYCODE_RWIN, KEYCODE_DEL_PAD, KEYCODE_PAUSE,*/

/* The format for each systems key constants is RETROK_$(TAG) and KEYCODE_$(TAG)*/
/* EMIT1(TAG): The tag value is the same between libretro and MAME*/
/* EMIT2(RTAG, MTAG): The tag value is different between the two*/
/* EXITX(TAG): MAME has no equivalent key.*/

#define EMIT2(RETRO, KEY) {(char*)#RETRO, RETROK_##RETRO, KEYCODE_##KEY}
#define EMIT1(KEY) {(char*)#KEY, RETROK_##KEY, KEYCODE_##KEY}
#define EMITX(KEY) {(char*)#KEY, RETROK_##KEY, KEYCODE_OTHER}

const struct KeyboardInfo retroKeys[] =
{
    EMIT1(BACKSPACE),
    EMIT1(TAB),
    EMITX(CLEAR),
    
    EMIT1(BACKSPACE),
    EMIT1(TAB),
    EMITX(CLEAR),
    EMIT2(RETURN, ENTER),
    EMITX(PAUSE),
    EMIT2(ESCAPE, ESC),
    EMIT1(SPACE),
    EMITX(EXCLAIM),
    EMIT2(QUOTEDBL, TILDE),
    EMITX(HASH),
    EMITX(DOLLAR),
    EMITX(AMPERSAND),
    EMIT1(QUOTE),
    EMITX(LEFTPAREN),
    EMITX(RIGHTPAREN),
    EMIT1(ASTERISK),
    EMIT2(PLUS, EQUALS),
    EMIT1(COMMA),
    EMIT1(MINUS),
    EMITX(PERIOD),
    EMIT1(SLASH),
    
    EMIT1(0), EMIT1(1), EMIT1(2), EMIT1(3), EMIT1(4), EMIT1(5), EMIT1(6), EMIT1(7), EMIT1(8), EMIT1(9),
    
    EMIT1(COLON),
    EMITX(SEMICOLON),
    EMITX(LESS),
    EMITX(EQUALS),
    EMITX(GREATER),
    EMITX(QUESTION),
    EMITX(AT),
    EMITX(LEFTBRACKET),
    EMIT1(BACKSLASH),
    EMITX(RIGHTBRACKET),
    EMITX(CARET),
    EMITX(UNDERSCORE),
    EMITX(BACKQUOTE),
    
    EMIT2(a, A), EMIT2(b, B), EMIT2(c, C), EMIT2(d, D), EMIT2(e, E), EMIT2(f, F),
    EMIT2(g, G), EMIT2(h, H), EMIT2(i, I), EMIT2(j, J), EMIT2(k, K), EMIT2(l, L),
    EMIT2(m, M), EMIT2(n, N), EMIT2(o, O), EMIT2(p, P), EMIT2(q, Q), EMIT2(r, R),
    EMIT2(s, S), EMIT2(t, T), EMIT2(u, U), EMIT2(v, V), EMIT2(w, W), EMIT2(x, X),
    EMIT2(y, Y), EMIT2(z, Z),
    
    EMIT2(DELETE, DEL),

    EMIT2(KP0, 0_PAD), EMIT2(KP1, 1_PAD), EMIT2(KP2, 2_PAD), EMIT2(KP3, 3_PAD),
    EMIT2(KP4, 4_PAD), EMIT2(KP5, 5_PAD), EMIT2(KP6, 6_PAD), EMIT2(KP7, 7_PAD),
    EMIT2(KP8, 8_PAD), EMIT2(KP9, 9_PAD),
    
    EMITX(KP_PERIOD),
    EMIT2(KP_DIVIDE, SLASH_PAD),
    EMITX(KP_MULTIPLY),
    EMIT2(KP_MINUS, MINUS_PAD),
    EMIT2(KP_PLUS, PLUS_PAD),
    EMIT2(KP_ENTER, ENTER_PAD),
    EMITX(KP_EQUALS),

    EMIT1(UP), EMIT1(DOWN), EMIT1(RIGHT), EMIT1(LEFT),
    EMIT1(INSERT), EMIT1(HOME), EMIT1(END), EMIT2(PAGEUP, PGUP), EMIT2(PAGEDOWN, PGDN),

    EMIT1(F1), EMIT1(F2), EMIT1(F3), EMIT1(F4), EMIT1(F5), EMIT1(F6),
    EMIT1(F7), EMIT1(F8), EMIT1(F9), EMIT1(F10), EMIT1(F11), EMIT1(F12),
    EMITX(F13), EMITX(F14), EMITX(F15),

    EMIT1(NUMLOCK),
    EMIT1(CAPSLOCK),
    EMIT2(SCROLLOCK, SCRLOCK),
    EMIT1(RSHIFT), EMIT1(LSHIFT), EMIT2(RCTRL, RCONTROL), EMIT2(LCTRL, LCONTROL), EMIT1(RALT), EMIT1(LALT),
    EMITX(RMETA), EMITX(LMETA), EMITX(LSUPER), EMITX(RSUPER),
    
    EMITX(MODE),
    EMITX(COMPOSE),

    EMITX(HELP),
    EMIT2(PRINT, PRTSCR),
    EMITX(SYSREQ),
    EMITX(BREAK),
    EMIT1(MENU),
    EMITX(POWER),
    EMITX(EURO),
    EMITX(UNDO),

    {0, 0, 0}
};
