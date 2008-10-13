/*
 *  File:       beam.cc
 *  Summary:    Functions related to ranged attacks.
 *  Written by: Linley Henzell
 *
 *  Modified for Crawl Reference by $Author$ on $Date$
 */

#include "AppHdr.h"
#include "beam.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <iostream>
#include <set>
#include <algorithm>

#ifdef DOS
#include <dos.h>
#include <conio.h>
#endif

#include "externs.h"

#include "cio.h"
#include "cloud.h"
#include "delay.h"
#include "effects.h"
#include "enum.h"
#include "fight.h"
#include "item_use.h"
#include "it_use2.h"
#include "items.h"
#include "itemname.h"
#include "itemprop.h"
#include "message.h"
#include "misc.h"
#include "monplace.h"
#include "monstuff.h"
#include "mon-util.h"
#include "mstuff2.h"
#include "mutation.h"
#include "ouch.h"
#include "player.h"
#include "religion.h"
#include "skills.h"
#include "spells1.h"
#include "spells3.h"
#include "spells4.h"
#include "state.h"
#include "stuff.h"
#include "terrain.h"
#include "traps.h"
#include "tutorial.h"
#include "view.h"
#include "xom.h"

#include "tiles.h"

#define BEAM_STOP       1000        // all beams stopped by subtracting this
                                    // from remaining range

static FixedArray < bool, 19, 19 > explode_map;

// Helper functions (some of these should probably be public).
static bool _affects_wall(const bolt &beam, int wall_feature);
static bool _isBouncy(bolt &beam, unsigned char gridtype);
static int _beam_source(const bolt &beam);
static std::string _beam_zapper(const bolt &beam);
static bool _beam_term_on_target(bolt &beam, const coord_def& p);
static void _beam_explodes(bolt &beam, const coord_def& p);
static int  _affect_wall(bolt &beam, const coord_def& p);
static int  _affect_place_clouds(bolt &beam, const coord_def& p);
static void _affect_place_explosion_clouds(bolt &beam, const coord_def& p);
static int  _affect_player(bolt &beam, item_def *item = NULL,
                           bool affect_items = true);
static int  _affect_monster(bolt &beam, monsters *mon, item_def *item = NULL);
static mon_resist_type _affect_monster_enchantment(bolt &beam, monsters *mon);
static void _beam_paralyses_monster( bolt &pbolt, monsters *monster );
static void _beam_petrifies_monster( bolt &pbolt, monsters *monster );
static int  _range_used_on_hit(bolt &beam);
static void _explosion1(bolt &pbolt);
static void _explosion_map(bolt &beam, const coord_def& p,
                           int count, int dir, int r);
static void _explosion_cell(bolt &beam, const coord_def& p, bool drawOnly,
                            bool affect_items = true);

static void _ench_animation(int flavour, const monsters *mon = NULL,
                            bool force = false);
static void _zappy(zap_type z_type, int power, bolt &pbolt);
static bool _nasty_beam(monsters *mon, const bolt &beam);
static bool _nice_beam(monsters *mon, const bolt &beam);

static std::set<std::string> beam_message_cache;

static bool _beam_is_blockable(bolt &pbolt)
{
    // BEAM_ELECTRICITY is added here because chain lighting is not
    // a true beam (stops at the first target it gets to and redirects
    // from there)... but we don't want it shield blockable.
    return (!pbolt.is_beam && !pbolt.is_explosion
            && pbolt.flavour != BEAM_ELECTRICITY);
}

// Kludge to suppress multiple redundant messages for a single beam.
static void _beam_mpr(msg_channel_type channel, const char *s, ...)
{
    va_list args;
    va_start(args, s);

    char buf[500];
    vsnprintf(buf, sizeof buf, s, args);

    va_end(args);

    std::string message = buf;
    if (beam_message_cache.find(message) == beam_message_cache.end())
        mpr(message.c_str(), channel);

    beam_message_cache.insert(message);
}

static kill_category _whose_kill(const bolt &beam)
{
    if (YOU_KILL(beam.thrower))
        return (KC_YOU);
    else if (MON_KILL(beam.thrower))
    {
        if (beam.beam_source == ANON_FRIENDLY_MONSTER)
            return (KC_FRIENDLY);
        if (!invalid_monster_index(beam.beam_source))
        {
            const monsters *mon = &menv[beam.beam_source];
            if (mons_friendly(mon))
                return (KC_FRIENDLY);
        }
    }
    return (KC_OTHER);
}

// A simple animated flash from Rupert Smith (expanded to be more
// generic).
void zap_animation(int colour, const monsters *mon, bool force)
{
    coord_def p = you.pos();

    // Default to whatever colour magic is today.
    if (colour == -1)
        colour = element_colour(EC_MAGIC);

    if (mon)
    {
        if (!force && !player_monster_visible( mon ))
            return;

        p = mon->pos();
    }

    if (!see_grid(p))
        return;

    const coord_def drawp = grid2view(p);

    if (in_los_bounds(drawp))
    {
#ifdef USE_TILE
        tiles.add_overlay(p, tileidx_zap(colour));
#else
        view_update();
        cgotoxy(drawp.x, drawp.y, GOTO_DNGN);
        put_colour_ch(colour, dchar_glyph(DCHAR_FIRED_ZAP));
#endif

        update_screen();
        delay(50);
    }
}

// Special front function for zap_animation to interpret enchantment flavours.
static void _ench_animation( int flavour, const monsters *mon, bool force )
{
    const int elem = (flavour == BEAM_HEALING)       ? EC_HEAL :
                     (flavour == BEAM_PAIN)          ? EC_UNHOLY :
                     (flavour == BEAM_DISPEL_UNDEAD) ? EC_HOLY :
                     (flavour == BEAM_POLYMORPH)     ? EC_MUTAGENIC :
                     (flavour == BEAM_TELEPORT
                        || flavour == BEAM_BANISH
                        || flavour == BEAM_BLINK)    ? EC_WARP
                                                     : EC_ENCHANT;

    zap_animation( element_colour( elem ), mon, force );
}

static void _beam_set_default_values(bolt &beam, int power)
{
    if (beam.range <= 0)
        beam.range = LOS_RADIUS;
    beam.hit            = 0;                 // default for "0" beams (I think)
    beam.damage         = dice_def( 1, 0 );  // default for "0" beams (I think)
    beam.type           = 0;                 // default for "0" beams
    beam.flavour        = BEAM_MAGIC;        // default for "0" beams
    beam.ench_power     = power;
    beam.obvious_effect = false;
    beam.is_beam        = false;             // default for all beams.
    beam.is_tracer      = false;             // default for all player beams
    beam.thrower        = KILL_YOU_MISSILE;  // missile from player
    beam.aux_source.clear();                 // additional source info, unused
}

// If needs_tracer is true, we need to check the beam path for friendly
// monsters for *player beams* only! If allies are found, the player is
// prompted to stop or continue.
bool zapping(zap_type ztype, int power, bolt &pbolt, bool needs_tracer,
             std::string msg)
{

#if DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS, "zapping: power=%d", power );
#endif

    _beam_set_default_values(pbolt, power);

    // For player bolts, check whether tracer goes through friendlies.
    // NOTE: Whenever zapping() is called with a randomized value for power
    // (or effect), player_tracer should be called directly with the highest
    // power possible respecting current skill, experience level etc.

    if (needs_tracer && pbolt.thrower == KILL_YOU_MISSILE
        && !player_tracer(ztype, power, pbolt))
    {
        return (false);
    }

    // Fill in the bolt structure.
    _zappy( ztype, power, pbolt );

    if (!msg.empty())
        mpr(msg.c_str());

    if (ztype == ZAP_LIGHTNING)
        noisy(25, you.pos(), "You hear a mighty clap of thunder!");

    if (ztype == ZAP_DIGGING)
        pbolt.aimed_at_spot = false;

    fire_beam(pbolt);

    return (true);
}

// Returns true if the path is considered "safe", and false if there are
// monsters in the way the player doesn't want to hit.
// NOTE: Doesn't check for the player being hit by a rebounding lightning bolt.
bool player_tracer( zap_type ztype, int power, bolt &pbolt, int range)
{
    // Non-controlleable during confusion.
    // (We'll shoot in a different direction anyway.)
    if (you.confused())
        return (true);

    _beam_set_default_values(pbolt, power);
    pbolt.name = "unimportant";

    pbolt.is_tracer      = true;
    pbolt.source         = you.pos();
    pbolt.can_see_invis  = player_see_invis();
    pbolt.smart_monster  = true;
    pbolt.attitude       = ATT_FRIENDLY;

    // Init tracer variables.
    pbolt.foe_count      = pbolt.fr_count = 0;
    pbolt.foe_power      = pbolt.fr_power = 0;
    pbolt.fr_helped      = pbolt.fr_hurt  = 0;
    pbolt.foe_helped     = pbolt.foe_hurt = 0;
    pbolt.foe_ratio      = 100;
    pbolt.beam_cancelled = false;
    pbolt.dont_stop_foe  = pbolt.dont_stop_fr = pbolt.dont_stop_player = false;

    fire_beam(pbolt);

    // Should only happen if the player answered 'n' to one of those
    // "Fire through friendly?" prompts.
    if (pbolt.beam_cancelled)
    {
#if DEBUG_DIAGNOSTICS
        mprf(MSGCH_DIAGNOSTICS, "%s", "Beam cancelled.");
#endif
        canned_msg(MSG_OK);
        you.turn_is_over = false;
        return (false);
    }

    // Set to non-tracing for actual firing.
    pbolt.is_tracer = false;
    return (true);
}

dice_def calc_dice( int num_dice, int max_damage )
{
    dice_def    ret( num_dice, 0 );

    if (num_dice <= 1)
    {
        ret.num  = 1;
        ret.size = max_damage;
    }
    else if (max_damage <= num_dice)
    {
        ret.num  = max_damage;
        ret.size = 1;
    }
    else
    {
        // Divide the damage among the dice, and add one
        // occasionally to make up for the fractions. -- bwr
        ret.size  = max_damage / num_dice;
        ret.size += x_chance_in_y(max_damage % num_dice, num_dice);
    }

    return (ret);
}

template<typename T>
struct power_deducer
{
    virtual T operator()(int pow) const = 0;
    virtual ~power_deducer() {}
};

typedef power_deducer<int> tohit_deducer;

template<int adder, int mult_num = 0, int mult_denom = 1>
struct tohit_calculator : public tohit_deducer
{
    int operator()(int pow) const
    {
        return adder + (pow * mult_num) / mult_denom;
    }
};

typedef power_deducer<dice_def> dam_deducer;

template<int numdice, int adder, int mult_num, int mult_denom>
struct dicedef_calculator : public dam_deducer
{
    dice_def operator()(int pow) const
    {
        return dice_def(numdice, adder + (pow * mult_num) / mult_denom);
    }
};

template<int numdice, int adder, int mult_num, int mult_denom>
struct calcdice_calculator : public dam_deducer
{
    dice_def operator()(int pow) const
    {
        return calc_dice(numdice, adder + (pow * mult_num) / mult_denom);
    }
};

struct zap_info
{
    zap_type ztype;
    const char* name;           // NULL means handled specially
    int power_cap;
    dam_deducer* damage;
    tohit_deducer* tohit;       // Enchantments have power modifier here
    int colour;
    bool is_enchantment;
    beam_type flavour;
    dungeon_char_type glyph;
    bool always_obvious;
    bool can_beam;
    bool is_explosion;
};

const zap_info zap_data[] = {

    {
        ZAP_FLAME,
        "puff of flame",
        50,
        new dicedef_calculator<2, 4, 1, 10>,
        new tohit_calculator<8, 1, 10>,
        RED,
        false,
        BEAM_FIRE,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false
    },

    {
        ZAP_FROST,
        "puff of frost",
        50,
        new dicedef_calculator<2, 4, 1, 10>,
        new tohit_calculator<8, 1, 10>,
        WHITE,
        false,
        BEAM_COLD,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false
    },

    {
        ZAP_SLOWING,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_SLOW,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_HASTING,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_HASTE,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_MAGIC_DARTS,
        "magic dart",
        25,
        new dicedef_calculator<1, 3, 1, 5>,
        new tohit_calculator<AUTOMATIC_HIT>,
        LIGHTMAGENTA,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false
    },
    
    {
        ZAP_HEALING,
        "0",
        100,
        new dicedef_calculator<1, 7, 1, 3>,
        NULL,
        BLACK,
        true,
        BEAM_HEALING,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_PARALYSIS,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_PARALYSIS,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_FIRE,
        "bolt of fire",
        200,
        new calcdice_calculator<6, 18, 2, 3>,
        new tohit_calculator<10, 1, 25>,
        RED,
        false,
        BEAM_FIRE,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },
  
    {
        ZAP_COLD,
        "bolt of cold",
        200,
        new calcdice_calculator<6, 18, 2, 3>,
        new tohit_calculator<10, 1, 25>,
        WHITE,
        false,
        BEAM_COLD,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },
        
    {
        ZAP_CONFUSION,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_CONFUSION,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_INVISIBILITY,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_INVISIBILITY,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_DIGGING,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_DIGGING,
        DCHAR_SPACE,
        false,
        true,
        false
    },

    {
        ZAP_FIREBALL,
        "fireball",
        200,
        new calcdice_calculator<3, 10, 1, 2>,
        new tohit_calculator<40>,
        RED,
        false,
        BEAM_FIRE,
        DCHAR_FIRED_ZAP,
        false,
        false,
        true
    },

    {
        ZAP_TELEPORTATION,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_TELEPORT,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_LIGHTNING,
        "bolt of lightning",
        200,
        new calcdice_calculator<1, 10, 3, 5>,
        new tohit_calculator<7, 1, 40>,
        LIGHTCYAN,
        false,
        BEAM_ELECTRICITY,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },

    {
        ZAP_POLYMORPH_OTHER,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_POLYMORPH,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_VENOM_BOLT,
        "bolt of poison",
        200,
        new calcdice_calculator<4, 15, 1, 2>,
        new tohit_calculator<8, 1, 20>,
        LIGHTGREEN,
        false,
        BEAM_POISON,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },
    
    {
        ZAP_NEGATIVE_ENERGY,
        "bolt of negative energy",
        200,
        new calcdice_calculator<4, 15, 3, 5>,
        new tohit_calculator<8, 1, 20>,
        DARKGREY,
        false,
        BEAM_NEG,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },

    {
        ZAP_CRYSTAL_SPEAR,
        "crystal spear",
        200,
        new calcdice_calculator<10, 23, 1, 1>,
        new tohit_calculator<10, 1, 15>,
        WHITE,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_MISSILE,
        true,
        false,
        false
    },

    {
        ZAP_BEAM_OF_ENERGY,
        "narrow beam of energy",
        1000,
        new calcdice_calculator<12, 40, 3, 2>,
        new tohit_calculator<1>,
        YELLOW,
        false,
        BEAM_ENERGY,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },

    {
        ZAP_MYSTIC_BLAST,
        "orb of energy",
        100,
        new calcdice_calculator<2, 15, 2, 5>,
        new tohit_calculator<10, 1, 7>,
        LIGHTMAGENTA,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false
    },

    {
        ZAP_ENSLAVEMENT,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_CHARM,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_PAIN,
        "0",
        100,
        new dicedef_calculator<1, 4, 1,5>,
        new tohit_calculator<0, 7, 2>,
        BLACK,
        true,
        BEAM_PAIN,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_STICKY_FLAME,
        "sticky flame",
        100,
        new dicedef_calculator<2, 3, 1, 12>,
        new tohit_calculator<11, 1, 10>,
        RED,
        false,
        BEAM_FIRE,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false
    },

    {
        ZAP_DISPEL_UNDEAD,
        "0",
        100,
        new calcdice_calculator<3, 20, 3, 4>,
        new tohit_calculator<0, 3, 2>,
        BLACK,
        true,
        BEAM_DISPEL_UNDEAD,
        DCHAR_SPACE,
        false,
        false,
        false
    },


    {
        ZAP_CLEANSING_FLAME,
        "golden flame",
        200,
        new calcdice_calculator<2, 20, 2, 3>,
        new tohit_calculator<150>,
        YELLOW,
        false,
        BEAM_HOLY,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },


    {
        ZAP_BONE_SHARDS,
        "spray of bone shards",
        // Incoming power is highly dependant on mass (see spells3.cc).
        // Basic function is power * 15 + mass...  with the largest
        // available mass (3000) we get a power of 4500 at a power
        // level of 100 (for 3d20).
        10000,
        new dicedef_calculator<3, 2, 1, 250>,
        new tohit_calculator<8, 1, 100>,
        LIGHTGREY,
        false,
        BEAM_MAGIC,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },

    {
        ZAP_BANISHMENT,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_BANISH,
        DCHAR_SPACE,
        false,
        false,
        false
    },


    {
        ZAP_DEGENERATION,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_DEGENERATE,
        DCHAR_SPACE,
        false,
        false,
        false
    },


    {
        ZAP_STING,
        "sting",
        25,
        new dicedef_calculator<1, 3, 1, 5>,
        new tohit_calculator<8, 1, 5>,
        GREEN,
        false,
        BEAM_POISON,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false
    },


    {
        ZAP_HELLFIRE,
        "hellfire",
        200,
        new calcdice_calculator<3, 10, 3, 4>,
        new tohit_calculator<20, 1, 10>,
        RED,
        false,
        BEAM_HELLFIRE,
        DCHAR_FIRED_ZAP,
        true,
        false,
        true
    },

    {
        ZAP_IRON_BOLT,
        "iron bolt",
        200,
        new calcdice_calculator<9, 15, 3, 4>,
        new tohit_calculator<7, 1, 15>,
        LIGHTCYAN,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_MISSILE,
        true,
        false,
        false
    },

    {
        ZAP_STRIKING,
        "force bolt",
        25,
        new dicedef_calculator<1, 5, 0, 1>,
        new tohit_calculator<8, 1, 10>,
        BLACK,
        false,
        BEAM_MMISSILE,
        DCHAR_SPACE,
        true,
        false,
        false
    },

    {
        ZAP_STONE_ARROW,
        "stone arrow",
        50,
        new dicedef_calculator<2, 5, 1, 7>,
        new tohit_calculator<8, 1, 10>,
        LIGHTGREY,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_MISSILE,
        true,
        false,
        false
    },

    {
        ZAP_ELECTRICITY,
        "zap",
        25,
        new dicedef_calculator<1, 3, 1, 4>,
        new tohit_calculator<8, 1, 7>,
        LIGHTCYAN,
        false,
        BEAM_ELECTRICITY,             // beams & reflects
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },

    {
        ZAP_ORB_OF_ELECTRICITY,
        "orb of electricity",
        200,
        new calcdice_calculator<0, 15, 4, 5>,
        new tohit_calculator<40>,
        LIGHTBLUE,
        false,
        BEAM_ELECTRICITY,
        DCHAR_FIRED_ZAP,
        true,
        false,
        true
    },

    {
        ZAP_SPIT_POISON,
        "splash of poison",
        50,
        new dicedef_calculator<1, 4, 1, 2>,
        new tohit_calculator<5, 1, 6>,
        GREEN,
        false,
        BEAM_POISON,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false
    },

    {
        ZAP_DEBUGGING_RAY,
        "debugging ray",
        10000,
        new dicedef_calculator<1500, 1, 0, 1>,
        new tohit_calculator<1500>,
        WHITE,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_DEBUG,
        true,
        false,
        false
    },

    {
        ZAP_BREATHE_FIRE,
        "fiery breath",
        50,
        new dicedef_calculator<3, 4, 1, 3>,
        new tohit_calculator<8, 1, 6>,
        RED,
        false,
        BEAM_FIRE,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },


    {
        ZAP_BREATHE_FROST,
        "freezing breath",
        50,
        new dicedef_calculator<3, 4, 1, 3>,
        new tohit_calculator<8, 1, 6>,
        WHITE,
        false,
        BEAM_COLD,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },

    {
        ZAP_BREATHE_ACID,
        "acid",
        50,
        new dicedef_calculator<3, 3, 1, 3>,
        new tohit_calculator<5, 1, 6>,
        YELLOW,
        false,
        BEAM_ACID,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },

    {
        ZAP_BREATHE_POISON,
        "poison gas",
        50,
        new dicedef_calculator<3, 2, 1, 6>,
        new tohit_calculator<6, 1, 6>,
        GREEN,
        false,
        BEAM_POISON,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },

    {
        ZAP_BREATHE_POWER,
        "bolt of energy",
        50,
        new dicedef_calculator<3, 3, 1, 3>,
        new tohit_calculator<5, 1, 6>,
        BLUE,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },

    {
        ZAP_ENSLAVE_UNDEAD,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_ENSLAVE_UNDEAD,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_AGONY,
        "0agony",
        100,
        NULL,
        new tohit_calculator<0, 5, 1>,
        BLACK,
        true,
        BEAM_PAIN,
        DCHAR_SPACE,
        false,
        false,
        false
    },


    {
        ZAP_DISRUPTION,
        "0",
        100,
        new dicedef_calculator<1, 4, 1, 5>,
        new tohit_calculator<0, 3, 1>,
        BLACK,
        true,
        BEAM_DISINTEGRATION,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_DISINTEGRATION,
        "0",
        100,
        new calcdice_calculator<3, 15, 3, 4>,
        new tohit_calculator<0, 5, 2>,
        BLACK,
        true,
        BEAM_DISINTEGRATION,
        DCHAR_SPACE,
        false,
        true,
        false
    },
    
    {
        ZAP_BREATHE_STEAM,
        "ball of steam",
        50,
        new dicedef_calculator<3, 4, 1, 5>,
        new tohit_calculator<10, 1, 10>,
        LIGHTGREY,
        false,
        BEAM_STEAM,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },

    {
        ZAP_CONTROL_DEMON,
        "0",
        100,
        NULL,
        new tohit_calculator<0, 3, 2>,
        BLACK,
        true,
        BEAM_ENSLAVE_DEMON,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_ORB_OF_FRAGMENTATION,
        "metal orb",
        200,
        new calcdice_calculator<3, 30, 3, 4>,
        new tohit_calculator<20>,
        CYAN,
        false,
        BEAM_FRAG,
        DCHAR_FIRED_ZAP,
        false,
        false,
        true
    },

    {
        ZAP_ICE_BOLT,
        "bolt of ice",
        100,
        new calcdice_calculator<3, 10, 1, 2>,
        new tohit_calculator<9, 1, 12>,
        WHITE,
        false,
        BEAM_ICE,
        DCHAR_FIRED_ZAP,
        false,
        false,
        false
    },

    {                           // ench_power controls radius
        ZAP_ICE_STORM,
        "great blast of cold",
        200,
        new calcdice_calculator<7, 22, 1, 1>,
        new tohit_calculator<20, 1, 10>,
        BLUE,
        false,
        BEAM_ICE,
        DCHAR_FIRED_ZAP,
        true,
        false,
        true
    },

    {
        ZAP_BACKLIGHT,
        "0",
        100,
        NULL,
        NULL,
        BLUE,
        true,
        BEAM_BACKLIGHT,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_SLEEP,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_SLEEP,
        DCHAR_SPACE,
        false,
        false,
        false
    },

    {
        ZAP_FLAME_TONGUE,
        "flame",
        25,
        new dicedef_calculator<1, 8, 1, 4>,
        new tohit_calculator<7, 1, 6>,
        RED,
        false,
        BEAM_FIRE,
        DCHAR_FIRED_BOLT,
        true,
        false,
        false
    },

    {
        ZAP_SANDBLAST,
        "rocky blast",
        50,
        new dicedef_calculator<2, 4, 1, 3>,
        new tohit_calculator<13, 1, 10>,
        BROWN,
        false,
        BEAM_FRAG,
        DCHAR_FIRED_BOLT,
        true,
        false,
        false
    },

    {
        ZAP_SMALL_SANDBLAST,
        "blast of sand",
        25,
        new dicedef_calculator<1, 8, 1, 4>,
        new tohit_calculator<8, 1, 5>,
        BROWN,
        false,
        BEAM_FRAG,
        DCHAR_FIRED_BOLT,
        true,
        false,
        false
    },

    {
        ZAP_MAGMA,
        "bolt of magma",
        200,
        new calcdice_calculator<4, 10, 3, 5>,
        new tohit_calculator<8, 1, 25>,
        RED,
        false,
        BEAM_LAVA,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false
    },

    {
        ZAP_POISON_ARROW,
        "poison arrow",
        200,
        new calcdice_calculator<4, 15, 1, 1>,
        new tohit_calculator<5, 1, 10>,
        LIGHTGREEN,
        false,
        BEAM_POISON_ARROW,             // extra damage
        DCHAR_FIRED_MISSILE,
        true,
        false,
        false
    },

    {
        ZAP_PETRIFY,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_PETRIFY,
        DCHAR_SPACE,
        false,
        false,
        false
    }
};


// Need to see zapping() for default values not set within this function {dlb}
static void _zappy( zap_type z_type, int power, bolt &pbolt )
{
    const zap_info* zinfo = NULL;

    // Find the appropriate zap info.
    for (unsigned int i = 0; i < ARRAYSZ(zap_data); ++i)
    {
        if (zap_data[i].ztype == z_type)
        {
            zinfo = &zap_data[i];
            break;
        }
    }

    // None found?
    if (zinfo == NULL)
    {
#ifdef DEBUG_DIAGNOSTICS
        mprf(MSGCH_ERROR, "Couldn't find zap type %d", z_type);
#endif
        return;
    }
    
    // Fill
    pbolt.name = zinfo->name;
    pbolt.flavour = zinfo->flavour;
    pbolt.colour = zinfo->colour;
    pbolt.type = dchar_glyph(zinfo->glyph);
    pbolt.obvious_effect = zinfo->always_obvious;
    pbolt.is_beam = zinfo->can_beam;
    pbolt.is_explosion = zinfo->is_explosion;

    if (zinfo->power_cap > 0)
        power = std::min(zinfo->power_cap, power);

    ASSERT(zinfo->is_enchantment == pbolt.is_enchantment());

    if (zinfo->is_enchantment)
    {
        pbolt.ench_power = (zinfo->tohit ? (*zinfo->tohit)(power) : power);
        pbolt.hit = AUTOMATIC_HIT;
    }
    else
    {
        pbolt.hit = (*zinfo->tohit)(power);
        if (wearing_amulet(AMU_INACCURACY))
            pbolt.hit = std::max(0, pbolt.hit - 5);
    }

    if (zinfo->damage)
        pbolt.damage = (*zinfo->damage)(power);

    // One special case
    if (z_type == ZAP_ICE_STORM)
        pbolt.ench_power     = power;              // used for radius
}

// Affect monster in wall unless it can shield itself using the wall.
// The wall will always shield the monster if the beam bounces off the
// wall, and a monster can't use a metal wall to shield itself from
// electricity.
static bool _affect_mon_in_wall(bolt &pbolt, item_def *item,
                                const coord_def& where)
{
    UNUSED(item);

    int mid = mgrd(where);

    if (mid == NON_MONSTER)
        return (false);

    if (pbolt.is_enchantment()
        || (!pbolt.is_explosion && !pbolt.is_big_cloud
            && (grd(where) == DNGN_METAL_WALL
                || pbolt.flavour != BEAM_ELECTRICITY)))
    {
        if (!mons_wall_shielded(&menv[mid]))
            return (true);
    }

    return (false);
}

/*
 * Beam pseudo code:
 *
 * 1. Calculate stepx and stepy - algorithms depend on finding a step axis
 *    which results in a line of rise 1 or less (ie 45 degrees or less)
 * 2. Calculate range.  Tracers always have max range, otherwise the beam
 *    will have somewhere between range and rangeMax
 * 3. Loop tracing out the line:
 *      3a. Check for walls and wall affecting beams
 *      3b. If no valid move is found, try a fuzzy move
 *      3c. If no valid move is yet found, try bouncing
 *      3d. If no valid move or bounce is found, break
 *      4. Check for beam termination on target
 *      5. Affect the cell which the beam just moved into -> affect()
 *      6. Decrease remaining range appropriately
 *      7. Check for early out due to aimed_at_feet
 *      8. Draw the beam
 * 9. Drop an object where the beam 'landed'
 *10. Beams explode where the beam 'landed'
 *11. If no message generated yet, send "nothing happens" (enchantments only)
 *
 */

void fire_beam(bolt &pbolt, item_def *item, bool drop_item)
{
    bool beamTerminate;     // Has beam been 'stopped' by something?
    coord_def &testpos(pbolt.pos);
    bool did_bounce = false;
    cursor_control coff(false);

    // [ds] Forcing the beam out of explosion phase here - currently
    // no caller relies on the beam already being in_explosion_phase.
    // This fixes beams being in explosion after use as a tracer.
    pbolt.in_explosion_phase = false;

    beam_message_cache.clear();

#ifdef USE_TILE
    int tile_beam = -1;

    if (item && !pbolt.is_tracer && pbolt.flavour == BEAM_MISSILE)
    {
        const coord_def diff = pbolt.target - pbolt.source;
        tile_beam = tileidx_item_throw(*item, diff.x, diff.y);
    }
#endif

#if DEBUG_DIAGNOSTICS
    if (pbolt.flavour != BEAM_LINE_OF_SIGHT)
    {
        mprf( MSGCH_DIAGNOSTICS, "%s%s%s [%s] (%d,%d) to (%d,%d): "
              "ty=%d col=%d flav=%d hit=%d dam=%dd%d range=%d",
              (pbolt.is_beam) ? "beam" : "missile",
              (pbolt.is_explosion) ? "*" :
              (pbolt.is_big_cloud) ? "+" : "",
              (pbolt.is_tracer) ? " tracer" : "",
              pbolt.name.c_str(),
              pbolt.source.x, pbolt.source.y,
              pbolt.target.x, pbolt.target.y,
              pbolt.type, pbolt.colour, pbolt.flavour,
              pbolt.hit, pbolt.damage.num, pbolt.damage.size,
              pbolt.range);
    }
#endif

    // init
    pbolt.aimed_at_feet = (pbolt.target == pbolt.source);
    pbolt.msg_generated = false;

    ray_def ray;

    if (pbolt.chose_ray)
        ray = pbolt.ray;
    else
    {
        ray.fullray_idx = -1;   // to quiet valgrind
        find_ray( pbolt.source, pbolt.target, true, ray, 0, true );
    }

    if (!pbolt.aimed_at_feet)
        ray.advance_through(pbolt.target);

    // Give chance for beam to affect one cell even if aimed_at_feet.
    beamTerminate = false;

    // Setup range.
    int rangeRemaining = pbolt.range;

    // Before we start drawing the beam, turn buffering off.
#ifdef WIN32CONSOLE
    bool oldValue = true;
    if (!pbolt.is_tracer)
        oldValue = set_buffering(false);
#endif
    while (!beamTerminate)
    {
        testpos = ray.pos();

        // Shooting through clouds affects accuracy.
        if (env.cgrid(testpos) != EMPTY_CLOUD)
            pbolt.hit = std::max(pbolt.hit - 2, 0);

        // See if tx, ty is blocked by something.
        if (grid_is_solid(grd(testpos)))
        {
            // First, check to see if this beam affects walls.
            if (_affects_wall(pbolt, grd(testpos)))
            {
                // Should we ever get a tracer with a wall-affecting
                // beam (possible I suppose), we'll quit tracing now.
                if (!pbolt.is_tracer)
                    rangeRemaining -= affect(pbolt, testpos, item);

                // If it's still a wall, quit.
                if (grid_is_solid(grd(testpos)))
                    break;      // breaks from line tracing
            }
            else
            {
                // BEGIN bounce case.  Bouncing protects any monster
                // in the wall.
                if (!_isBouncy(pbolt, grd(testpos)))
                {
                    // Affect any monster that might be in the wall.
                    rangeRemaining -= affect(pbolt, testpos, item);

                    do
                    {
                        ray.regress();
                    }
                    while (grid_is_solid(grd(ray.pos())));

                    testpos = ray.pos();
                    break;          // breaks from line tracing
                }

                did_bounce = true;

                // bounce
                do
                {
                    do
                        ray.regress();
                    while (grid_is_solid(grd(ray.pos())));

                    ray.advance_and_bounce();
                    rangeRemaining -= 2;
                }
                while (rangeRemaining > 0 && grid_is_solid(grd(ray.pos())));

                if (rangeRemaining < 1)
                    break;

                testpos = ray.pos();
            } // end else - beam doesn't affect walls
        } // endif - are we in a wall wall?

        // At this point, if grd(testpos) is still a wall, we
        // couldn't find any path: bouncy, fuzzy, or not - so break.
        if (grid_is_solid(grd(testpos)))
            break;

        // Check for "target termination"
        // occurs when beam can be targetted at empty
        // cell (e.g. a mage wants an explosion to happen
        // between two monsters).

        // In this case, don't affect the cell - players and
        // monsters have no chance to dodge or block such
        // a beam, and we want to avoid silly messages.
        if (testpos == pbolt.target)
            beamTerminate = _beam_term_on_target(pbolt, testpos);

        // Affect the cell, except in the special case noted
        // above -- affect() will early out if something gets
        // hit and the beam is type 'term on target'.
        if (!beamTerminate || !pbolt.is_explosion)
        {
            // Random beams: randomize before affect().
            bool random_beam = false;
            if (pbolt.flavour == BEAM_RANDOM)
            {
                random_beam = true;
                pbolt.flavour = static_cast<beam_type>(
                                    random_range(BEAM_FIRE, BEAM_ACID));
            }

            if (!pbolt.affects_nothing)
                rangeRemaining -= affect(pbolt, testpos, item);

            if (random_beam)
            {
                pbolt.flavour = BEAM_RANDOM;
                pbolt.effect_known = false;
            }
        }

        if (pbolt.beam_cancelled)
            return;

        // Always decrease range by 1.
        rangeRemaining--;

        // Check for range termination.
        if (rangeRemaining <= 0)
            beamTerminate = true;

        // Special case - beam was aimed at feet.
        if (pbolt.aimed_at_feet)
            beamTerminate = true;

        // Actually draw the beam/missile/whatever,
        // if the player can see the cell.
        if (!pbolt.is_tracer && !pbolt.is_enchantment() && see_grid(testpos))
        {
            // We don't clean up the old position.
            // First, most people like to see the full path,
            // and second, it is hard to do it right with
            // respect to killed monsters, cloud trails, etc.

            // Draw new position.
            coord_def drawpos = grid2view(testpos);

#ifdef USE_TILE
            if (tile_beam == -1)
                tile_beam = tileidx_bolt(pbolt);

            if (tile_beam != -1 && in_los_bounds(drawpos))
            {
                tiles.add_overlay(testpos, tile_beam);
                delay(15);
            }
            else
#endif
            // bounds check
            if (in_los_bounds(drawpos))
            {
#ifndef USE_TILE
                cgotoxy(drawpos.x, drawpos.y);
                put_colour_ch(
                    pbolt.colour == BLACK ? random_colour() : pbolt.colour,
                    pbolt.type );
#endif
                // Get curses to update the screen so we can see the beam.
                update_screen();

                delay(15);

#ifdef MISSILE_TRAILS_OFF
                // mv: It's not optimal but is usually enough.
                if (!pbolt.is_beam || pbolt.is_enchantment())
                    viewwindow(true, false);
#endif
            }

        }

        if (!did_bounce)
            ray.advance_through(pbolt.target);
        else
            ray.advance(true);
    } // end- while !beamTerminate

    // The beam has finished, and terminated at tx, ty.

    // Leave an object, if applicable.
    if (drop_item && item)
        beam_drop_object(pbolt, item, testpos);

    ASSERT(!drop_item || item);

    // Check for explosion.  NOTE that for tracers, we have to make a copy
    // of target co-ords and then reset after calling this -- tracers should
    // never change any non-tracers fields in the beam structure. -- GDL
    coord_def targetcopy = pbolt.target;

    _beam_explodes(pbolt, testpos);

    if (pbolt.is_tracer)
    {
        pbolt.target = targetcopy;
    }

    // Canned msg for enchantments that affected no-one, but only if the
    // enchantment is yours.
    if (pbolt.is_enchantment())
    {
        if (!pbolt.is_tracer && !pbolt.msg_generated && !pbolt.obvious_effect
            && YOU_KILL(pbolt.thrower))
        {
            canned_msg(MSG_NOTHING_HAPPENS);
        }
    }

    if (!pbolt.is_tracer && !invalid_monster_index(pbolt.beam_source))
    {
        if (pbolt.foe_hurt == 0 && pbolt.fr_hurt > 0)
            xom_is_stimulated(128);
        else if (pbolt.foe_helped > 0 && pbolt.fr_helped == 0)
            xom_is_stimulated(128);

        // Allow friendlies to react to projectiles, except when in
        // sanctuary when pet_target can only be explicitly changed by
        // the player.
        const monsters *mon = &menv[pbolt.beam_source];
        if (pbolt.foe_hurt > 0 && !mons_wont_attack(mon)
            && you.pet_target == MHITNOT && env.sanctuary_time <= 0)
        {
            you.pet_target = pbolt.beam_source;
        }
    }

    // That's it!
#ifdef WIN32CONSOLE
    if (!pbolt.is_tracer)
        set_buffering(oldValue);
#endif
}


// Returns damage taken by a monster from a "flavoured" (fire, ice, etc.)
// attack -- damage from clouds and branded weapons handled elsewhere.
int mons_adjust_flavoured(monsters *monster, bolt &pbolt, int hurted,
                          bool doFlavouredEffects)
{
    // If we're not doing flavoured effects, must be preliminary
    // damage check only.
    // Do not print messages or apply any side effects!
    int resist = 0;
    int original = hurted;

    switch (pbolt.flavour)
    {
    case BEAM_FIRE:
    case BEAM_STEAM:
        hurted = resist_adjust_damage(
                    monster,
                    pbolt.flavour,
                    (pbolt.flavour == BEAM_FIRE) ? monster->res_fire()
                                                 : monster->res_steam(),
                    hurted, true);

        if (!hurted)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " appears unharmed.");
        }
        else if (original > hurted)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " resists.");
        }
        else if (original < hurted)
        {
            if (mons_is_icy(monster))
            {
                if (doFlavouredEffects)
                    simple_monster_message(monster, " melts!");
            }
            else
            {
                if (doFlavouredEffects)
                {
                    if (pbolt.flavour == BEAM_FIRE)
                        simple_monster_message(monster,
                                               " is burned terribly!");
                    else
                        simple_monster_message(monster,
                                               " is scalded terribly!");
                }
            }
        }
        break;

    case BEAM_COLD:
        hurted = resist_adjust_damage(monster, pbolt.flavour,
                                      monster->res_cold(),
                                      hurted, true);
        if (!hurted)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " appears unharmed.");
        }
        else if (original > hurted)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " resists.");
        }
        else if (original < hurted)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " is frozen!");
        }
        break;

    case BEAM_ELECTRICITY:
        hurted = resist_adjust_damage(monster, pbolt.flavour,
                                      monster->res_elec(),
                                      hurted, true);
        if (!hurted)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " appears unharmed.");
        }
        break;

    case BEAM_ACID:
        hurted = resist_adjust_damage(monster, pbolt.flavour,
                                      mons_res_acid(monster),
                                      hurted, true);
        if (!hurted)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " appears unharmed.");
        }
        break;

    case BEAM_POISON:
    {
        int res = mons_res_poison(monster);
        hurted  = resist_adjust_damage(monster, pbolt.flavour, res,
                                       hurted, true);
        if (!hurted && res > 0)
        {
            if (doFlavouredEffects)
                simple_monster_message( monster, " appears unharmed." );
        }
        else if (res <= 0 && doFlavouredEffects && !one_chance_in(3))
            poison_monster( monster, _whose_kill(pbolt) );

        break;
    }

    case BEAM_POISON_ARROW:
        hurted = resist_adjust_damage(monster, pbolt.flavour,
                                      mons_res_poison(monster),
                                      hurted);
        if (hurted < original)
        {
            if (doFlavouredEffects)
            {
                simple_monster_message( monster, " partially resists." );

                // Poison arrow can poison any living thing regardless of
                // poison resistance. -- bwr
                if (mons_has_lifeforce(monster))
                    poison_monster( monster, _whose_kill(pbolt), 2, true );
            }
        }
        else if (doFlavouredEffects)
            poison_monster( monster, _whose_kill(pbolt), 4 );

        break;

    case BEAM_NEG:
        if (mons_res_negative_energy(monster))
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " appears unharmed.");

            hurted = 0;
        }
        else
        {
            // Early out for tracer/no side effects.
            if (!doFlavouredEffects)
                return (hurted);

            simple_monster_message(monster, " is drained.");
            pbolt.obvious_effect = true;

            if (YOU_KILL(pbolt.thrower))
            {
                did_god_conduct(DID_NECROMANCY, 2 + random2(3),
                                pbolt.effect_known);
            }

            if (one_chance_in(5))
            {
                monster->hit_dice--;
                monster->experience = 0;
            }

            monster->max_hit_points -= 2 + random2(3);
            monster->hit_points     -= 2 + random2(3);

            if (monster->hit_points >= monster->max_hit_points)
                monster->hit_points = monster->max_hit_points;

            if (monster->hit_dice < 1)
                monster->hit_points = 0;
        }                       // end else
        break;

    case BEAM_MIASMA:
        if (mons_res_negative_energy(monster) == 3)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " appears unharmed.");

            hurted = 0;
        }
        else
        {
            // Early out for tracer/no side effects.
            if (!doFlavouredEffects)
                return (hurted);

            if (mons_res_poison( monster ) <= 0)
                poison_monster( monster, _whose_kill(pbolt) );

            if (one_chance_in( 3 + 2 * mons_res_negative_energy(monster) ))
            {
                bolt beam;
                beam.flavour = BEAM_SLOW;
                mons_ench_f2( monster, beam );
            }
        }
        break;

    case BEAM_HOLY:             // flame of cleansing
        // Cleansing flame doesn't hurt holy monsters or monsters your
        // god wouldn't like to be hurt.
        if (mons_is_holy(monster)
            || you.religion == GOD_SHINING_ONE
               && is_unchivalric_attack(&you, monster, monster)
            || is_good_god(you.religion)
               && (is_follower(monster) || mons_neutral(monster)))
        {
            hurted = 0;
        }
        else if (mons_is_unholy(monster))
            hurted = (hurted * 3) / 2;
        else if (!mons_is_evil(monster))
            hurted /= 2;

        if (doFlavouredEffects)
        {
            simple_monster_message(monster, (hurted == 0) ?
                " appears unharmed." : " writhes in agony!");
        }

        break;

    case BEAM_ICE:
        // ice - about 50% of damage is cold, other 50% is impact and
        // can't be resisted (except by AC, of course)
        hurted = resist_adjust_damage(monster, pbolt.flavour,
                                      monster->res_cold(), hurted,
                                      true);
        if (hurted < original)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " partially resists.");
        }
        else if (hurted > original)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " is frozen!");
        }
        break;

    case BEAM_LAVA:
        hurted = resist_adjust_damage(monster, pbolt.flavour,
                                      monster->res_fire(), hurted, true);

        if (hurted < original)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " partially resists.");
        }
        else if (hurted > original)
        {
            if (mons_is_icy(monster))
            {
                if (doFlavouredEffects)
                    simple_monster_message(monster, " melts!");
            }
            else
            {
                if (doFlavouredEffects)
                    simple_monster_message(monster, " is burned terribly!");
            }
        }
        break;
    default:
        break;
    }                           // end of switch

    if (pbolt.name == "hellfire")
    {
        resist = mons_res_fire(monster);
        if (resist > 2)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " appears unharmed.");

            hurted = 0;
        }
        else if (resist > 0)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " partially resists.");

            hurted /= 2;
        }
        else if (resist < 0)
        {
            if (mons_is_icy(monster))
            {
                if (doFlavouredEffects)
                    simple_monster_message(monster, " melts!");
            }
            else
            {
                if (doFlavouredEffects)
                    simple_monster_message(monster, " is burned terribly!");
            }

            hurted *= 12;       // hellfire
            hurted /= 10;
        }
    }

    return (hurted);
}                               // end mons_adjust_flavoured()

static bool _monster_resists_mass_enchantment(monsters *monster,
                                              enchant_type wh_enchant,
                                              int pow)
{
    // Assuming that the only mass charm is control undead.
    if (wh_enchant == ENCH_CHARM)
    {
        if (mons_friendly(monster))
            return (true);

        if (mons_class_holiness(monster->type) != MH_UNDEAD)
            return (true);

        if (check_mons_resist_magic( monster, pow ))
        {
            simple_monster_message(monster,
                                   mons_immune_magic(monster)? " is unaffected."
                                                             : " resists.");
            return (true);
        }
    }
    else if (wh_enchant == ENCH_CONFUSION
             || mons_holiness(monster) == MH_NATURAL)
    {
        if (wh_enchant == ENCH_CONFUSION
            && !mons_class_is_confusable(monster->type))
        {
            return (true);
        }

        if (check_mons_resist_magic( monster, pow ))
        {
            simple_monster_message(monster,
                                   mons_immune_magic(monster)? " is unaffected."
                                                             : " resists.");
            return (true);
        }
    }
    else  // trying to enchant an unnatural creature doesn't work
    {
        simple_monster_message(monster, " is unaffected.");
        return (true);
    }

    return (false);
}

// Enchants all monsters in player's sight.
// If m_succumbed is non-NULL, will be set to the number of monsters that
// were enchanted. If m_attempted is non-NULL, will be set to the number of
// monsters that we tried to enchant.
bool mass_enchantment( enchant_type wh_enchant, int pow, int origin,
                       int *m_succumbed, int *m_attempted )
{
    int i;                      // loop variable {dlb}
    bool msg_generated = false;
    monsters *monster;

    if (m_succumbed)
        *m_succumbed = 0;
    if (m_attempted)
        *m_attempted = 0;

    viewwindow(false, false);

    if (pow > 200)
        pow = 200;

    const kill_category kc = (origin == MHITYOU ? KC_YOU : KC_OTHER);

    for (i = 0; i < MAX_MONSTERS; i++)
    {
        monster = &menv[i];

        if (monster->type == -1 || !mons_near(monster))
            continue;

        if (monster->has_ench(wh_enchant))
            continue;

        if (m_attempted)
            ++*m_attempted;

        if (_monster_resists_mass_enchantment(monster, wh_enchant, pow))
            continue;

        if (monster->add_ench(mon_enchant(wh_enchant, 0, kc)))
        {
            if (m_succumbed)
                ++*m_succumbed;

            if (player_monster_visible( monster ))
            {
                // turn message on
                msg_generated = true;
                switch (wh_enchant)
                {
                case ENCH_FEAR:
                    simple_monster_message(monster,
                                           " looks frightened!");
                    break;
                case ENCH_CONFUSION:
                    simple_monster_message(monster,
                                           " looks rather confused.");
                    break;
                case ENCH_CHARM:
                    simple_monster_message(monster,
                                           " submits to your will.");
                    break;
                default:
                    // oops, I guess not!
                    msg_generated = false;
                }
            }

            // Extra check for fear (monster needs to reevaluate behaviour).
            if (wh_enchant == ENCH_FEAR)
                behaviour_event( monster, ME_SCARE, origin );
        }
    }                           // end "for i"

    if (!msg_generated)
        canned_msg(MSG_NOTHING_HAPPENS);

    return (msg_generated);
}                               // end mass_enchantment()

// Monster has probably failed save, now it gets enchanted somehow.
// * Returns MON_RESIST if monster is unaffected due to magic resist.
// * Returns MON_UNAFFECTED if monster is immune to enchantment.
// * Returns MON_AFFECTED in all other cases (already enchanted, etc).
mon_resist_type mons_ench_f2(monsters *monster, bolt &pbolt)
{
    switch (pbolt.flavour)      // put in magic resistance
    {
    case BEAM_SLOW:
        // try to remove haste, if monster is hasted
        if (monster->del_ench(ENCH_HASTE))
        {
            if (simple_monster_message(monster,
                                       " is no longer moving quickly."))
            {
                pbolt.obvious_effect = true;
            }

            return (MON_AFFECTED);
        }

        // not hasted, slow it
        if (!monster->has_ench(ENCH_SLOW)
            && !mons_is_stationary(monster)
            && monster->add_ench(mon_enchant(ENCH_SLOW, 0, _whose_kill(pbolt))))
        {
            if (!mons_is_paralysed(monster) && !mons_is_petrified(monster)
                && simple_monster_message(monster, " seems to slow down."))
            {
                pbolt.obvious_effect = true;
            }
        }
        return (MON_AFFECTED);

    case BEAM_HASTE:
        if (monster->del_ench(ENCH_SLOW))
        {
            if (simple_monster_message(monster, " is no longer moving slowly."))
                pbolt.obvious_effect = true;

            return (MON_AFFECTED);
        }

        // Not slowed, haste it.
        if (!monster->has_ench(ENCH_HASTE)
            && !mons_is_stationary(monster)
            && monster->add_ench(ENCH_HASTE))
        {
            if (!mons_is_paralysed(monster) && !mons_is_petrified(monster)
                && simple_monster_message(monster, " seems to speed up."))
            {
                pbolt.obvious_effect = true;
            }
        }
        return (MON_AFFECTED);

    case BEAM_HEALING:
        if (YOU_KILL(pbolt.thrower))
        {
            if (cast_healing(5 + pbolt.damage.roll(), monster->pos()) > 0)
                pbolt.obvious_effect = true;
            pbolt.msg_generated = true; // to avoid duplicate "nothing happens"
        }
        else if (heal_monster( monster, 5 + pbolt.damage.roll(), false ))
        {
            if (monster->hit_points == monster->max_hit_points)
            {
                if (simple_monster_message(monster,
                                           "'s wounds heal themselves!"))
                {
                    pbolt.obvious_effect = true;
                }
            }
            else if (simple_monster_message(monster, " is healed somewhat."))
                pbolt.obvious_effect = true;
        }
        return (MON_AFFECTED);

    case BEAM_PARALYSIS:
        _beam_paralyses_monster(pbolt, monster);
        return (MON_AFFECTED);

    case BEAM_PETRIFY:
        _beam_petrifies_monster(pbolt, monster);
        return (MON_AFFECTED);

    case BEAM_CONFUSION:
        if (!mons_class_is_confusable(monster->type))
            return (MON_UNAFFECTED);

        if (monster->add_ench( mon_enchant(ENCH_CONFUSION, 0,
                               _whose_kill(pbolt)) ))
        {
            // Put in an exception for things you won't notice becoming
            // confused.
            if (simple_monster_message(monster, " appears confused."))
                pbolt.obvious_effect = true;
        }
        return (MON_AFFECTED);

    case BEAM_INVISIBILITY:
    {
        // Store the monster name before it becomes an "it" -- bwr
        const std::string monster_name = monster->name(DESC_CAP_THE);

        if (!monster->has_ench(ENCH_INVIS)
            && monster->add_ench(ENCH_INVIS))
        {
            // A casting of invisibility erases backlight.
            monster->del_ench(ENCH_BACKLIGHT);

            // Can't use simple_monster_message() here, since it checks
            // for visibility of the monster (and it's now invisible).
            // -- bwr
            if (mons_near( monster ))
            {
                mprf("%s flickers %s",
                     monster_name.c_str(),
                     player_monster_visible(monster) ? "for a moment."
                                                     : "and vanishes!" );

                if (!player_monster_visible(monster))
                {
                    // Don't swap weapons just because you can't see it anymore!
                    you.attribute[ATTR_WEAPON_SWAP_INTERRUPTED] = 0;

                    // Also turn off autopickup.
                    Options.autopickup_on = false;
                    mpr("Deactivating autopickup; reactivate with Ctrl-A.",
                        MSGCH_WARN);

                    if (Options.tutorial_left)
                    {
                        learned_something_new(TUT_INVISIBLE_DANGER);
                        Options.tut_seen_invisible = you.num_turns;
                    }
                }
            }

            pbolt.obvious_effect = true;
        }
        return (MON_AFFECTED);
    }
    case BEAM_CHARM:
        if (player_will_anger_monster(monster))
        {
            simple_monster_message(monster, " is repulsed!");
            return (MON_OTHER);
        }

        if (monster->add_ench(ENCH_CHARM))
        {
            // Put in an exception for fungi, plants and other things
            // you won't notice becoming charmed.
            if (simple_monster_message(monster, " is charmed."))
                pbolt.obvious_effect = true;
        }
        return (MON_AFFECTED);

    default:
        break;
    }

    return (MON_AFFECTED);
}

// degree is ignored.
static void _slow_monster(monsters *mon, int /* degree */)
{
    bolt beam;
    beam.flavour = BEAM_SLOW;
    mons_ench_f2(mon, beam);
}

static void _beam_paralyses_monster(bolt &pbolt, monsters *monster)
{
    if (!monster->has_ench(ENCH_PARALYSIS)
        && monster->add_ench(ENCH_PARALYSIS)
        && (!monster->has_ench(ENCH_PETRIFIED)
            || monster->has_ench(ENCH_PETRIFYING)))
    {
        if (simple_monster_message(monster, " suddenly stops moving!"))
            pbolt.obvious_effect = true;

        mons_check_pool(monster, pbolt.killer(), pbolt.beam_source);
    }
}


// Petrification works in two stages. First the monster is slowed down in
// all of its actions and cannot move away (petrifying), and when that times
// out it remains properly petrified (no movement or actions). The second
// part is similar to paralysis, except that insubstantial monsters can't be
// affected and that stabbing damage is drastically reduced.
static void _beam_petrifies_monster(bolt &pbolt, monsters *monster)
{
    int petrifying = monster->has_ench(ENCH_PETRIFYING);
    if (monster->has_ench(ENCH_PETRIFIED))
    {
        // If the petrifying is not yet finished, we can force it to happen
        // right away by casting again. Otherwise, the spell has no further
        // effect.
        if (petrifying > 0)
        {
            monster->del_ench(ENCH_PETRIFYING, true);
            if (!monster->has_ench(ENCH_PARALYSIS)
                && simple_monster_message(monster, " stops moving altogether!"))
            {
                pbolt.obvious_effect = true;
            }
        }
    }
    else if (monster->add_ench(ENCH_PETRIFIED)
             && !monster->has_ench(ENCH_PARALYSIS))
    {
        // Add both the petrifying and the petrified enchantment. The former
        // will run out sooner and result in plain petrification behaviour.
        monster->add_ench(ENCH_PETRIFYING);
        if (simple_monster_message(monster, " is moving more slowly."))
            pbolt.obvious_effect = true;

        mons_check_pool(monster, pbolt.killer(), pbolt.beam_source);
    }
}

// Returns true if the curare killed the monster.
bool curare_hits_monster( const bolt &beam,  monsters *monster,
                          kill_category who, int levels )
{
    const bool res_poison = mons_res_poison(monster) > 0;

    poison_monster(monster, who, levels, false);

    if (!mons_res_asphyx(monster))
    {
        int hurted = roll_dice(2, 6);

        // Note that the hurtage is halved by poison resistance.
        if (res_poison)
            hurted /= 2;

        if (hurted)
        {
            simple_monster_message(monster, " convulses.");
            monster->hurt(beam.agent(), hurted, BEAM_POISON);
        }

        if (monster->alive())
            _slow_monster(monster, levels);
    }

    // Deities take notice.
    if (who == KC_YOU)
        did_god_conduct( DID_POISON, 5 + random2(3) );

    return (!monster->alive());
}

// Actually poisons a monster (w/ message).
bool poison_monster( monsters *monster,
                     kill_category from_whom,
                     int levels,
                     bool force,
                     bool verbose)
{
    if (!monster->alive())
        return (false);

    if (!levels || (!force && mons_res_poison(monster) > 0))
        return (false);

    const mon_enchant old_pois = monster->get_ench(ENCH_POISON);
    monster->add_ench( mon_enchant(ENCH_POISON, levels, from_whom) );
    const mon_enchant new_pois = monster->get_ench(ENCH_POISON);

    // Actually do the poisoning.
    // Note: order important here.
    if (verbose && new_pois.degree > old_pois.degree)
    {
        simple_monster_message( monster,
                                !old_pois.degree? " is poisoned."
                                                : " looks even sicker." );
    }

    // Finally, take care of deity preferences.
    if (from_whom == KC_YOU)
        did_god_conduct( DID_POISON, 5 + random2(3) );

    return (new_pois.degree > old_pois.degree);
}

// Actually napalms the player.
void sticky_flame_player()
{
    you.duration[DUR_LIQUID_FLAMES] += random2avg(7, 3) + 1;
}

// Actually napalms a monster (with message).
void sticky_flame_monster(int mn, kill_category who, int levels)
{
    monsters *monster = &menv[mn];

    if (!monster->alive())
        return;

    if (mons_res_sticky_flame(monster))
        return;

    if (monster->add_ench(mon_enchant(ENCH_STICKY_FLAME, levels, who)))
    {
        simple_monster_message(monster, " is covered in liquid flames!");
        behaviour_event(monster, ME_WHACK, who == KC_YOU ? MHITYOU : MHITNOT);
    }
}

//
//  Used by monsters in "planning" which spell to cast. Fires off a "tracer"
//  which tells the monster what it'll hit if it breathes/casts etc.
//
//  The output from this tracer function is four variables in the beam struct:
//  fr_count, foe_count: a count of how many friends and foes will (probably)
//  be hit by this beam
//  fr_power, foe_power: a measure of how many 'friendly' hit dice it will
//  affect, and how many 'unfriendly' hit dice.
//
//  Note that beam properties must be set, as the tracer will take them
//  into account, as well as the monster's intelligence.
//
void fire_tracer(const monsters *monster, bolt &pbolt, bool explode_only)
{
    // Don't fiddle with any input parameters other than tracer stuff!
    pbolt.is_tracer     = true;
    pbolt.source        = monster->pos();
    pbolt.beam_source   = monster_index(monster);
    pbolt.can_see_invis = mons_see_invis(monster);
    pbolt.smart_monster = (mons_intel(monster) >= I_NORMAL);
    pbolt.attitude      = mons_attitude(monster);

    // Init tracer variables.
    pbolt.foe_count     = pbolt.fr_count = 0;
    pbolt.foe_power     = pbolt.fr_power = 0;
    pbolt.fr_helped     = pbolt.fr_hurt  = 0;
    pbolt.foe_helped    = pbolt.foe_hurt = 0;

    // If there's a specifically requested foe_ratio, honour it.
    if (!pbolt.foe_ratio)
    {
        pbolt.foe_ratio     = 80;        // default - see mons_should_fire()

        // Foe ratio for summoning greater demons & undead -- they may be
        // summoned, but they're hostile and would love nothing better
        // than to nuke the player and his minions.
        if (mons_att_wont_attack(pbolt.attitude)
            && !mons_att_wont_attack(monster->attitude))
        {
            pbolt.foe_ratio = 25;
        }
    }

    // Fire!
    if (explode_only)
        explosion(pbolt, false, false, true, true, false);
    else
        fire_beam(pbolt);

    // Unset tracer flag (convenience).
    pbolt.is_tracer     = false;
}

bool check_line_of_sight( const coord_def& source, const coord_def& target )
{
    const int dist = grid_distance( source, target );

    // Can always see one square away.
    if (dist <= 1)
        return (true);

    // Currently we limit the range to 8.
    if (dist > MONSTER_LOS_RANGE)
        return (false);

    // Note that we are guaranteed to be within the player LOS range,
    // so fallback is unnecessary.
    ray_def ray;
    return find_ray( source, target, false, ray );
}

// When a mimic is hit by a ranged attack, it teleports away (the slow
// way) and changes its appearance - the appearance change is in
// monster_teleport() in mstuff2.cc.
void mimic_alert(monsters *mimic)
{
    if (!mimic->alive())
        return;

    bool should_id = !testbits(mimic->flags, MF_KNOWN_MIMIC)
                     && player_monster_visible(mimic) && mons_near(mimic);

    // If we got here, we at least got a resists message, if not
    // a full wounds printing. Thus, might as well id the mimic.
    if (mimic->has_ench(ENCH_TP))
    {
        if (should_id)
            mimic->flags |= MF_KNOWN_MIMIC;

        return;
    }

    const bool instant_tele = !one_chance_in(3);
    monster_teleport( mimic, instant_tele );

    // At least for this short while, we know it's a mimic.
    if (!instant_tele && should_id)
        mimic->flags |= MF_KNOWN_MIMIC;
}

static bool _isBouncy(bolt &beam, unsigned char gridtype)
{
    if (beam.is_enchantment())
        return (false);

    if (beam.flavour == BEAM_ELECTRICITY && gridtype != DNGN_METAL_WALL)
        return (true);

    if ((beam.flavour == BEAM_FIRE || beam.flavour == BEAM_COLD)
         && gridtype == DNGN_GREEN_CRYSTAL_WALL )
    {
        return (true);
    }
    return (false);
}

static void _beam_explodes(bolt &beam, const coord_def& p)
{
    cloud_type cl_type;

    // This will be the last thing this beam does.  Set target_x
    // and target_y to hold explosion co'ords.

    beam.target = p;

    // Generic explosion.
    if (beam.is_explosion)
    {
        _explosion1(beam);
        return;
    }

    if (beam.flavour >= BEAM_POTION_STINKING_CLOUD
        && beam.flavour <= BEAM_POTION_RANDOM)
    {
        switch (beam.flavour)
        {
        case BEAM_POTION_STINKING_CLOUD:
            beam.colour = GREEN;
            break;

        case BEAM_POTION_POISON:
            beam.colour = (coinflip() ? GREEN : LIGHTGREEN);
            break;

        case BEAM_POTION_MIASMA:
        case BEAM_POTION_BLACK_SMOKE:
            beam.colour = DARKGREY;
            break;

        case BEAM_POTION_STEAM:
        case BEAM_POTION_GREY_SMOKE:
            beam.colour = LIGHTGREY;
            break;

        case BEAM_POTION_FIRE:
            beam.colour = (coinflip() ? RED : LIGHTRED);
            break;

        case BEAM_POTION_COLD:
            beam.colour = (coinflip() ? BLUE : LIGHTBLUE);
            break;

        case BEAM_POTION_BLUE_SMOKE:
            beam.colour = LIGHTBLUE;
            break;

        case BEAM_POTION_PURP_SMOKE:
            beam.colour = MAGENTA;
            break;

        case BEAM_POTION_RANDOM:
        default:
            // Leave it the colour of the potion, the clouds will colour
            // themselves on the next refresh. -- bwr
            break;
        }

        _explosion1(beam);
        return;
    }

    if (beam.is_tracer)
        return;

    // cloud producer -- POISON BLAST
    if (beam.name == "blast of poison")
    {
        big_cloud(CLOUD_POISON, _whose_kill(beam), beam.killer(), p,
                  0, 7 + random2(5));
        return;
    }

    // cloud producer -- FOUL VAPOR (SWAMP DRAKE?)
    if (beam.name == "foul vapour")
    {
        cl_type = (beam.flavour == BEAM_MIASMA) ? CLOUD_MIASMA : CLOUD_STINK;
        big_cloud( cl_type, _whose_kill(beam), beam.killer(), p, 0, 9 );
        return;
    }

    if (beam.name == "freezing blast")
    {
        big_cloud( CLOUD_COLD, _whose_kill(beam), beam.killer(), p,
                   random_range(10, 15), 9 );
        return;
    }

    // special cases - orbs & blasts of cold
    if (beam.name == "orb of electricity"
        || beam.name == "metal orb"
        || beam.name == "great blast of cold")
    {
        _explosion1( beam );
        return;
    }

    // cloud producer only -- stinking cloud
    if (beam.name == "ball of vapour")
    {
        _explosion1( beam );
        return;
    }
}

static bool _beam_term_on_target(bolt &beam, const coord_def& p)
{
    if (beam.flavour == BEAM_LINE_OF_SIGHT)
    {
        if (beam.thrower != KILL_YOU_MISSILE)
            beam.foe_count++;
        return (true);
    }

    // Generic - all explosion-type beams can be targeted at empty space,
    // and will explode there.  This semantic also means that a creature
    // in the target cell will have no chance to dodge or block, so we
    // DON'T affect() the cell if this function returns true!

    if (beam.is_explosion || beam.is_big_cloud)
        return (true);

    // POISON BLAST
    if (beam.name == "blast of poison")
        return (true);

    // FOUL VAPOR (SWAMP DRAKE)
    if (beam.name == "foul vapour")
        return (true);

    // STINKING CLOUD
    if (beam.name == "ball of vapour")
        return (true);

    if (beam.aimed_at_spot && p == beam.target)
        return (true);

    return (false);
}

void beam_drop_object( bolt &beam, item_def *item, const coord_def& p )
{
    ASSERT( item != NULL );

    // Conditions: beam is missile and not tracer.
    if (beam.is_tracer || beam.flavour != BEAM_MISSILE)
        return;

    if (YOU_KILL(beam.thrower)
            && !thrown_object_destroyed(item, p, false)
        || MON_KILL(beam.thrower)
            && !mons_thrown_object_destroyed(item, p, false, beam.beam_source))
    {
        if (item->sub_type == MI_THROWING_NET)
        {
            // Player or monster on position is caught in net.
            if (you.pos() == p && you.attribute[ATTR_HELD]
                || mgrd(p) != NON_MONSTER &&
                   mons_is_caught(&menv[mgrd(p)]))
            {
                // If no trapping net found mark this one.
                if (get_trapping_net(p, true) == NON_ITEM)
                    set_item_stationary(*item);
            }
        }

        copy_item_to_grid( *item, p, 1 );
    }
}

// Returns true if the beam hits the player, fuzzing the beam if necessary
// for monsters without see invis firing tracers at the player.
static bool _found_player(const bolt &beam, const coord_def& p)
{
    const bool needs_fuzz = (beam.is_tracer && !beam.can_see_invis
                             && you.invisible() && !YOU_KILL(beam.thrower));
    const int dist = needs_fuzz? 2 : 0;

    return (grid_distance(p, you.pos()) <= dist);
}

int affect(bolt &beam, const coord_def& p, item_def *item, bool affect_items)
{
    // Extra range used by hitting something.
    int rangeUsed = 0;

    // Line of sight never affects anything.
    if (beam.flavour == BEAM_LINE_OF_SIGHT)
        return (0);

    if (grid_is_solid(grd(p)))
    {
        if (beam.is_tracer)          // Tracers always stop on walls.
            return (BEAM_STOP);

        if (_affects_wall(beam, grd(p)))
            rangeUsed += _affect_wall(beam, p);

        // If it's still a wall, quit - we can't do anything else to a
        // wall (but we still might be able to do something to any
        // monster inside the wall).  Otherwise effects (like clouds,
        // etc.) are still possible.
        if (grid_is_solid(grd(p)))
        {
            int mid = mgrd(p);
            if (mid != NON_MONSTER)
            {
                monsters *mon = &menv[mid];
                if (_affect_mon_in_wall(beam, NULL, p))
                    rangeUsed += _affect_monster( beam, mon, item );
                else if (you.can_see(mon))
                {
                    mprf("The %s protects %s from harm.",
                         raw_feature_description(grd(mon->pos())).c_str(),
                         mon->name(DESC_NOCAP_THE).c_str());
                }
            }

            return (rangeUsed);
        }
    }

    // grd(p) will NOT be a wall for the remainder of this function.

    // If not a tracer, affect items and place clouds.
    if (!beam.is_tracer)
    {
        if (affect_items)
        {
            const int burn_power = (beam.is_explosion) ? 5 :
                                        (beam.is_beam) ? 3 : 2;

            expose_items_to_element(beam.flavour, p, burn_power);
        }
        rangeUsed += _affect_place_clouds(beam, p);
    }

    // If player is at this location, try to affect unless term_on_target.
    if (_found_player(beam, p))
    {
        // Done this way so that poison blasts affect the target once (via
        // place_cloud) and explosion spells only affect the target once
        // (during the explosion phase, not an initial hit during the
        // beam phase).
        if (!beam.is_big_cloud
            && (!beam.is_explosion || beam.in_explosion_phase))
        {
            rangeUsed += _affect_player( beam, item, affect_items );
        }

        if (_beam_term_on_target(beam, p))
            return (BEAM_STOP);
    }

    // If there is a monster at this location, affect it.
    // Submerged monsters aren't really there. -- bwr
    int mid = mgrd(p);
    if (mid != NON_MONSTER)
    {
        monsters *mon = &menv[mid];
        const bool invisible = YOU_KILL(beam.thrower) && !you.can_see(mon);

        // Monsters submerged in shallow water can be targeted by beams
        // aimed at that spot.
        if (mon->alive()
            // Don't stop tracers on invisible monsters.
            && (!invisible || !beam.is_tracer)
            && (!mon->submerged()
                || beam.aimed_at_spot && beam.target == mon->pos()
                   && grd(mon->pos()) == DNGN_SHALLOW_WATER))
        {
            if (!beam.is_big_cloud
                && (!beam.is_explosion || beam.in_explosion_phase))
            {
                rangeUsed += _affect_monster( beam, &menv[mid], item );
            }

            if (_beam_term_on_target(beam, p))
                return (BEAM_STOP);
        }
    }

    return (rangeUsed);
}

static bool _is_fiery(const bolt &beam)
{
    return (beam.flavour == BEAM_FIRE || beam.flavour == BEAM_HELLFIRE
            || beam.flavour == BEAM_LAVA);
}

static bool _is_superhot(const bolt &beam)
{
    if (!_is_fiery(beam))
        return (false);

    return (beam.name == "bolt of fire"
            || beam.name == "bolt of magma"
            || beam.name.find("hellfire") != std::string::npos
               && beam.in_explosion_phase);
}

static bool _affects_wall(const bolt &beam, int wall)
{
    // digging
    if (beam.flavour == BEAM_DIGGING)
        return (true);

    // FIXME: There should be a better way to test for ZAP_DISRUPTION
    // vs. ZAP_DISINTEGRATION.
    if (beam.flavour == BEAM_DISINTEGRATION && beam.damage.num >= 3)
        return (true);

    if (_is_fiery(beam) && wall == DNGN_WAX_WALL)
        return (true);

    // eye of devastation?
    if (beam.flavour == BEAM_NUKE)
        return (true);

    return (false);
}

// Returns amount of extra range used up by affectation of this wall.
static int _affect_wall(bolt &beam, const coord_def& p)
{
    int rangeUsed = 0;

    // DIGGING
    if (beam.flavour == BEAM_DIGGING)
    {
        if (grd(p) == DNGN_STONE_WALL
            || grd(p) == DNGN_METAL_WALL
            || grd(p) == DNGN_PERMAROCK_WALL
            || grd(p) == DNGN_CLEAR_STONE_WALL
            || grd(p) == DNGN_CLEAR_PERMAROCK_WALL
            || !in_bounds(p))
        {
            return (0);
        }

        if (grd(p) == DNGN_ROCK_WALL || grd(p) == DNGN_CLEAR_ROCK_WALL)
        {
            grd(p) = DNGN_FLOOR;
            // Mark terrain as changed so travel excludes can be updated
            // as necessary.
            // XXX: This doesn't work for some reason: after digging
            //      the wrong grids are marked excluded.
            set_terrain_changed(p);

            // Blood does not transfer onto floor.
            if (is_bloodcovered(p))
                env.map(p).property = FPROP_NONE;

            if (!beam.msg_generated)
            {
                if (!silenced(you.pos()))
                {
                    mpr("You hear a grinding noise.", MSGCH_SOUND);
                    beam.obvious_effect = true;
                }

                beam.msg_generated = true;
            }
        }

        return (rangeUsed);
    }
    // END DIGGING EFFECT

    // FIRE effect
    if (_is_fiery(beam))
    {
        const int wgrd = grd(p);
        if (wgrd != DNGN_WAX_WALL)
            return (0);

        if (!_is_superhot(beam))
        {
            if (beam.flavour != BEAM_HELLFIRE)
            {
                if (see_grid(p))
                {
                    _beam_mpr(MSGCH_PLAIN,
                             "The wax appears to soften slightly.");
                }
                else if (player_can_smell())
                    _beam_mpr(MSGCH_PLAIN, "You smell warm wax.");
            }

            return (BEAM_STOP);
        }

        grd(p) = DNGN_FLOOR;
        if (see_grid(p))
            _beam_mpr(MSGCH_PLAIN, "The wax bubbles and burns!");
        else if (player_can_smell())
            _beam_mpr(MSGCH_PLAIN, "You smell burning wax.");

        place_cloud(CLOUD_FIRE, p, random2(10) + 15, _whose_kill(beam),
                    beam.killer());

        beam.obvious_effect = true;

        return (BEAM_STOP);
    }

    // NUKE / DISRUPT
    if (beam.flavour == BEAM_DISINTEGRATION || beam.flavour == BEAM_NUKE)
    {
        int targ_grid = grd(p);

        if ((targ_grid == DNGN_ROCK_WALL || targ_grid == DNGN_WAX_WALL
                 || targ_grid == DNGN_CLEAR_ROCK_WALL)
            && in_bounds(p))
        {
            grd(p) = DNGN_FLOOR;
            if (!silenced(you.pos()))
            {
                mpr("You hear a grinding noise.", MSGCH_SOUND);
                beam.obvious_effect = true;
            }
        }

        if (targ_grid == DNGN_ORCISH_IDOL
            || targ_grid == DNGN_GRANITE_STATUE)
        {
            grd(p) = DNGN_FLOOR;

            // Blood does not transfer onto floor.
            if (is_bloodcovered(p))
                env.map(p).property = FPROP_NONE;

            if (!silenced(you.pos()))
            {
                if (!see_grid( p ))
                    mpr("You hear a hideous screaming!", MSGCH_SOUND);
                else
                {
                    mpr("The statue screams as its substance crumbles away!",
                        MSGCH_SOUND);
                }
            }
            else if (see_grid(p))
                mpr("The statue twists and shakes as its substance crumbles away!");

            if (targ_grid == DNGN_ORCISH_IDOL
                && beam.beam_source == NON_MONSTER)
            {
                beogh_idol_revenge();
            }
            beam.obvious_effect = true;
        }

        return (BEAM_STOP);
    }

    return (rangeUsed);
}

static int _affect_place_clouds(bolt &beam, const coord_def& p)
{
    if (beam.in_explosion_phase)
    {
        _affect_place_explosion_clouds( beam, p );
        return (0);       // return value irrelevant for explosions
    }

    // check for CLOUD HITS
    if (env.cgrid(p) != EMPTY_CLOUD)     // hit a cloud
    {
        // polymorph randomly changes clouds in its path
        if (beam.flavour == BEAM_POLYMORPH)
        {
            env.cloud[ env.cgrid(p) ].type =
                static_cast<cloud_type>(1 + random2(8));
        }

        // now exit (all enchantments)
        if (beam.is_enchantment())
            return (0);

        int clouty = env.cgrid(p);

        // fire cancelling cold & vice versa
        if ((env.cloud[clouty].type == CLOUD_COLD
             && (beam.flavour == BEAM_FIRE
                 || beam.flavour == BEAM_LAVA))
            || (env.cloud[clouty].type == CLOUD_FIRE
                && beam.flavour == BEAM_COLD))
        {
            if (player_can_hear(p))
                mpr("You hear a sizzling sound!", MSGCH_SOUND);

            delete_cloud( clouty );
            return (5);
        }
    }

    // POISON BLAST
    if (beam.name == "blast of poison")
        place_cloud( CLOUD_POISON, p, random2(4) + 2, _whose_kill(beam),
                     beam.killer() );

    // FIRE/COLD over water/lava
    if (grd(p) == DNGN_LAVA && beam.flavour == BEAM_COLD
        || grid_is_watery(grd(p)) && _is_fiery(beam))
    {
        place_cloud( CLOUD_STEAM, p, 2 + random2(5), _whose_kill(beam),
                     beam.killer() );
    }

    if (grid_is_watery(grd(p)) && beam.flavour == BEAM_COLD
        && beam.damage.num * beam.damage.size > 35)
    {
        place_cloud( CLOUD_COLD, p, beam.damage.num * beam.damage.size / 30 + 1,
                     _whose_kill(beam), beam.killer() );
    }

    // GREAT BLAST OF COLD
    if (beam.name == "great blast of cold")
        place_cloud( CLOUD_COLD, p, random2(5) + 3, _whose_kill(beam),
                     beam.killer() );


    // BALL OF STEAM
    if (beam.name == "ball of steam")
        place_cloud( CLOUD_STEAM, p, random2(5) + 2, _whose_kill(beam),
                     beam.killer() );

    if (beam.flavour == BEAM_MIASMA)
        place_cloud( CLOUD_MIASMA, p, random2(5) + 2, _whose_kill(beam),
                     beam.killer() );

    // POISON GAS
    if (beam.name == "poison gas")
        place_cloud( CLOUD_POISON, p, random2(4) + 3, _whose_kill(beam),
                     beam.killer() );

    return (0);
}

static void _affect_place_explosion_clouds(bolt &beam, const coord_def& p)
{
    cloud_type cl_type;
    int duration;

    // First check: FIRE/COLD over water/lava.
    if (grd(p) == DNGN_LAVA && beam.flavour == BEAM_COLD
         || grid_is_watery(grd(p)) && _is_fiery(beam))
    {
        place_cloud( CLOUD_STEAM, p, 2 + random2(5), _whose_kill(beam) );
        return;
    }

    if (beam.flavour >= BEAM_POTION_STINKING_CLOUD
        && beam.flavour <= BEAM_POTION_RANDOM)
    {
        duration = roll_dice( 2, 3 + beam.ench_power / 20 );

        switch (beam.flavour)
        {
        case BEAM_POTION_STINKING_CLOUD:
        case BEAM_POTION_POISON:
        case BEAM_POTION_MIASMA:
        case BEAM_POTION_STEAM:
        case BEAM_POTION_FIRE:
        case BEAM_POTION_COLD:
        case BEAM_POTION_BLACK_SMOKE:
        case BEAM_POTION_GREY_SMOKE:
        case BEAM_POTION_BLUE_SMOKE:
        case BEAM_POTION_PURP_SMOKE:
            cl_type = beam2cloud(beam.flavour);
            break;

        case BEAM_POTION_RANDOM:
            switch (random2(11))
            {
            case 0:  cl_type = CLOUD_FIRE;           break;
            case 1:  cl_type = CLOUD_STINK;          break;
            case 2:  cl_type = CLOUD_COLD;           break;
            case 3:  cl_type = CLOUD_POISON;         break;
            case 4:  cl_type = CLOUD_BLACK_SMOKE;    break;
            case 5:  cl_type = CLOUD_GREY_SMOKE;     break;
            case 6:  cl_type = CLOUD_BLUE_SMOKE;     break;
            case 7:  cl_type = CLOUD_PURP_SMOKE;     break;
            default: cl_type = CLOUD_STEAM;          break;
            }
            break;

        default:
            cl_type = CLOUD_STEAM;
            break;
        }

        place_cloud( cl_type, p, duration, _whose_kill(beam) );
    }

    // then check for more specific explosion cloud types.
    if (beam.name == "ice storm")
        place_cloud( CLOUD_COLD, p, 2 + random2avg(5, 2), _whose_kill(beam) );

    if (beam.name == "stinking cloud")
    {
        duration =  1 + random2(4) + random2( (beam.ench_power / 50) + 1 );
        place_cloud( CLOUD_STINK, p, duration, _whose_kill(beam) );
    }

    if (beam.name == "great blast of fire")
    {
        duration = 1 + random2(5) + roll_dice( 2, beam.ench_power / 5 );

        if (duration > 20)
            duration = 20 + random2(4);

        place_cloud( CLOUD_FIRE, p, duration, _whose_kill(beam) );

        if (grd(p) == DNGN_FLOOR && mgrd(p) == NON_MONSTER
            && one_chance_in(4))
        {
            const god_type god =
                (crawl_state.is_god_acting()) ? crawl_state.which_god_acting()
                                              : GOD_NO_GOD;
            const beh_type att =
                _whose_kill(beam) == KC_OTHER ? BEH_HOSTILE : BEH_FRIENDLY;

            mons_place(
                mgen_data(MONS_FIRE_VORTEX, att, 2, p,
                          MHITNOT, 0, god));
        }
    }
}

// A little helper function to handle the calling of ouch()...
static void _beam_ouch(int dam, bolt &beam)
{
    // The order of this is important.
    if (YOU_KILL(beam.thrower) && beam.aux_source.empty())
    {
        ouch(dam, NON_MONSTER, KILLED_BY_TARGETTING);
    }
    else if (MON_KILL(beam.thrower))
    {
        if (beam.flavour == BEAM_SPORE)
            ouch(dam, beam.beam_source, KILLED_BY_SPORE);
        else
        {
            ouch(dam, beam.beam_source, KILLED_BY_BEAM,
                 beam.aux_source.c_str());
        }
    }
    else // KILL_MISC || (YOU_KILL && aux_source)
    {
        ouch(dam, beam.beam_source, KILLED_BY_WILD_MAGIC,
             beam.aux_source.c_str());
    }
}

// [ds] Apply a fuzz if the monster lacks see invisible and is trying to target
// an invisible player. This makes invisibility slightly more powerful.
static bool _fuzz_invis_tracer(bolt &beem)
{
    // Did the monster have a rough idea of where you are?
    int dist = grid_distance(beem.target, you.pos());

    // No, ditch this.
    if (dist > 2)
        return (false);

    const int beam_src = _beam_source(beem);
    if (beam_src != MHITNOT && beam_src != MHITYOU)
    {
        // Monsters that can sense invisible
        const monsters *mon = &menv[beam_src];
        if (mons_sense_invis(mon))
            return (!dist);
    }

    // Apply fuzz now.
    coord_def fuzz( random_range(-2, 2), random_range(-2, 2) );
    coord_def newtarget = beem.target + fuzz;

    if (in_bounds(newtarget))
        beem.target = newtarget;

    // Fire away!
    return (true);
}

// A first step towards to-hit sanity for beams. We're still being
// very kind to the player, but it should be fairer to monsters than
// 4.0.
bool test_beam_hit(int attack, int defence)
{
#ifdef DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS, "Beam attack: %d, defence: %d", attack, defence);
#endif
    return (attack == AUTOMATIC_HIT
            || random2(attack) >= random2avg(defence, 2));
}

static std::string _beam_zapper(const bolt &beam)
{
    const int beam_src = _beam_source(beam);
    if (beam_src == MHITYOU)
        return ("self");
    else if (beam_src == MHITNOT)
        return ("");
    else
        return menv[beam_src].name(DESC_PLAIN);
}

static bool _beam_is_harmless(bolt &beam, monsters *mon)
{
    // For enchantments, this is already handled in _nasty_beam().
    if (beam.is_enchantment())
        return (!_nasty_beam(mon, beam));

    // The others are handled here.
    switch (beam.flavour)
    {
    case BEAM_DIGGING:
        return (true);

    // Cleansing flame doesn't affect player's followers.
    case BEAM_HOLY:
        return (mons_is_holy(mon)
                || is_good_god(you.religion)
                   && ( is_follower(mon) || mons_neutral(mon) ));

    case BEAM_STEAM:
        return (mons_res_steam(mon) >= 3);

    case BEAM_FIRE:
        return (mons_res_fire(mon) >= 3);

    case BEAM_COLD:
        return (mons_res_cold(mon) >= 3);

    case BEAM_MIASMA:
    case BEAM_NEG:
        return (mons_res_negative_energy(mon) == 3);

    case BEAM_ELECTRICITY:
        return (mons_res_elec(mon) >= 3);

    case BEAM_POISON:
        return (mons_res_poison(mon) >= 3);

    case BEAM_ACID:
        return (mons_res_acid(mon) >= 3);

    default:
        return (false);
    }
}

static bool _beam_is_harmless_player(bolt &beam)
{
#ifdef DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS, "beam flavour: %d", beam.flavour);
#endif

    // Shouldn't happen anyway since enchantments are either aimed at self
    // (not prompted) or cast at monsters and don't explode or bounce.
    if (beam.is_enchantment())
        return (false);

    // The others are handled here.
    switch (beam.flavour)
    {
    case BEAM_DIGGING:
        return (true);

    // Cleansing flame doesn't affect player's followers.
    case BEAM_HOLY:
        return (is_good_god(you.religion));

    case BEAM_STEAM:
        return (player_res_steam(false) >= 3);

    case BEAM_MIASMA:
    case BEAM_NEG:
        return (player_prot_life(false) >= 3);

    case BEAM_POISON:
        return (player_res_poison(false));

    case BEAM_POTION_STINKING_CLOUD:
        return (player_res_poison(false) || player_mental_clarity(false));

    case BEAM_ELECTRICITY:
        return (player_res_electricity(false));

    case BEAM_FIRE:
    case BEAM_COLD:
    case BEAM_ACID:
        // Fire and ice can destroy inventory items, acid damage equipment.
        return (false);

    default:
        return (false);
    }
}

// Returns amount of extra range used up by affectation of the player.
static int _affect_player( bolt &beam, item_def *item, bool affect_items )
{
    // Digging -- don't care.
    if (beam.flavour == BEAM_DIGGING)
        return (0);

    // Check for tracer.
    if (beam.is_tracer)
    {
        // Check whether thrower can see player, unless thrower == player.
        if (YOU_KILL(beam.thrower))
        {
            // Don't ask if we're aiming at ourselves.
            if (!beam.aimed_at_feet && !beam.dont_stop_player
                && !_beam_is_harmless_player(beam))
            {
                if (yesno("That beam is likely to hit you. Continue anyway?",
                    false, 'n'))
                {
                    beam.fr_count += 1;
                    beam.fr_power += you.experience_level;
                    beam.dont_stop_player = true;
                }
                else
                {
                    beam.beam_cancelled = true;
                    return (BEAM_STOP);
                }
            }
        }
        else if (beam.can_see_invis || !you.invisible()
                 || _fuzz_invis_tracer(beam))
        {
            if (mons_att_wont_attack(beam.attitude))
            {
                beam.fr_count += 1;
                beam.fr_power += you.experience_level;
            }
            else
            {
                beam.foe_count++;
                beam.foe_power += you.experience_level;
            }
        }
        return (_range_used_on_hit(beam));
    }

    // Trigger an interrupt, so travel will stop on misses
    // which generate smoke.
    if (!YOU_KILL(beam.thrower))
        interrupt_activity(AI_MONSTER_ATTACKS);

    // BEGIN real beam code
    beam.msg_generated = true;

    // Use beamHit, NOT beam.hit, for modification of tohit.. geez!
    int beamHit = beam.hit;

    // Monsters shooting at an invisible player are very inaccurate.
    if (you.invisible() && !beam.can_see_invis)
        beamHit /= 2;

    if (!beam.is_enchantment())
    {
        if (!beam.is_explosion && !beam.aimed_at_feet)
        {
            // BEGIN BEAM/MISSILE
            int dodge = player_evasion();

            if (beam.is_beam)
            {
                // Beams can be dodged.
                if (player_light_armour(true)
                    && !beam.aimed_at_feet && coinflip())
                {
                    exercise(SK_DODGING, 1);
                }

                if (you.duration[DUR_DEFLECT_MISSILES])
                    beamHit = random2(beamHit * 2) / 3;
                else if (you.duration[DUR_REPEL_MISSILES]
                         || player_mutation_level(MUT_REPULSION_FIELD) == 3)
                {
                    beamHit -= random2(beamHit / 2);
                }

                if (!test_beam_hit(beamHit, dodge))
                {
                    mprf("The %s misses you.", beam.name.c_str());
                    return (0);           // no extra used by miss!
                }
            }
            else if (_beam_is_blockable(beam))
            {
                // Non-beams can be blocked or dodged.
                if (you.shield()
                    && !beam.aimed_at_feet
                    && player_shield_class() > 0)
                {
                    int exer = one_chance_in(3) ? 1 : 0;
                    const int hit = random2( beam.hit * 130 / 100
                                             + you.shield_block_penalty() );

                    const int block = you.shield_bonus();

#ifdef DEBUG_DIAGNOSTICS
                    mprf(MSGCH_DIAGNOSTICS, "Beamshield: hit: %d, block %d",
                         hit, block);
#endif
                    if (hit < block)
                    {
                        mprf( "You block the %s.", beam.name.c_str() );
                        you.shield_block_succeeded();
                        return (BEAM_STOP);
                    }

                    // Some training just for the "attempt".
                    if (coinflip())
                        exercise( SK_SHIELDS, exer );
                }

                if (player_light_armour(true) && !beam.aimed_at_feet
                    && coinflip())
                {
                    exercise(SK_DODGING, 1);
                }

                if (you.duration[DUR_DEFLECT_MISSILES])
                    beamHit = random2(beamHit / 2);
                else if (you.duration[DUR_REPEL_MISSILES]
                         || player_mutation_level(MUT_REPULSION_FIELD) == 3)
                {
                    beamHit = random2(beamHit);
                }

                // miss message
                if (!test_beam_hit(beamHit, dodge))
                {
                    mprf("The %s misses you.", beam.name.c_str());
                    return (0);
                }
            }
        }
    }
    else
    {
        bool nasty = true, nice = false;

        // BEGIN enchantment beam
        if (beam.flavour != BEAM_HASTE
            && beam.flavour != BEAM_INVISIBILITY
            && beam.flavour != BEAM_HEALING
            && beam.flavour != BEAM_POLYMORPH
            && beam.flavour != BEAM_DISPEL_UNDEAD
            && (beam.flavour != BEAM_TELEPORT && beam.flavour != BEAM_BANISH
                || !beam.aimed_at_feet)
            && you_resist_magic( beam.ench_power ))
        {
            bool need_msg = true;
            if (beam.thrower != KILL_YOU_MISSILE
                && !invalid_monster_index(beam.beam_source))
            {
                monsters *mon = &menv[beam.beam_source];
                if (!player_monster_visible(mon))
                {
                    mpr("Something tries to affect you, but you resist.");
                    need_msg = false;
                }
            }
            if (need_msg)
                canned_msg(MSG_YOU_RESIST);

            // You *could* have gotten a free teleportation in the Abyss,
            // but no, you resisted.
            if (beam.flavour != BEAM_TELEPORT && you.level_type == LEVEL_ABYSS)
                xom_is_stimulated(255);

            return (_range_used_on_hit(beam));
        }

        _ench_animation( beam.flavour );

        // these colors are misapplied - see mons_ench_f2() {dlb}
        switch (beam.flavour)
        {
        case BEAM_SLEEP:
            you.put_to_sleep(beam.ench_power);
            break;

        case BEAM_BACKLIGHT:
            if (!you.duration[DUR_INVIS])
            {
                if (you.duration[DUR_BACKLIGHT])
                    mpr("You glow brighter.");
                else
                    mpr("You are outlined in light.");

                you.duration[DUR_BACKLIGHT] += random_range(15, 35);
                if (you.duration[DUR_BACKLIGHT] > 250)
                    you.duration[DUR_BACKLIGHT] = 250;

                beam.obvious_effect = true;
            }
            else
            {
                mpr("You feel strangely conspicuous.");
                if ((you.duration[DUR_BACKLIGHT] += random_range(3, 5)) > 250)
                    you.duration[DUR_BACKLIGHT] = 250;
                beam.obvious_effect = true;
            }
            break;

        case BEAM_POLYMORPH:
            if (MON_KILL(beam.thrower))
            {
                mpr("Strange energies course through your body.");
                you.mutate();
                beam.obvious_effect = true;
            }
            else if (get_ident_type(OBJ_WANDS, WAND_POLYMORPH_OTHER)
                     == ID_KNOWN_TYPE)
            {
                mpr("This is polymorph other only!");
            }
            else
                canned_msg( MSG_NOTHING_HAPPENS );

            break;

        case BEAM_SLOW:
            potion_effect( POT_SLOWING, beam.ench_power );
            beam.obvious_effect = true;
            break;

        case BEAM_HASTE:
            potion_effect( POT_SPEED, beam.ench_power );
            contaminate_player( 1, beam.effect_known );
            beam.obvious_effect = true;
            nasty = false;
            nice  = true;
            break;

        case BEAM_HEALING:
            potion_effect( POT_HEAL_WOUNDS, beam.ench_power );
            beam.obvious_effect = true;
            nasty = false;
            nice  = true;
            break;

        case BEAM_PARALYSIS:
            potion_effect( POT_PARALYSIS, beam.ench_power );
            beam.obvious_effect = true;
            break;

        case BEAM_PETRIFY:
            you.petrify( beam.ench_power );
            beam.obvious_effect = true;
            break;

        case BEAM_CONFUSION:
            potion_effect( POT_CONFUSION, beam.ench_power );
            beam.obvious_effect = true;
            break;

        case BEAM_INVISIBILITY:
            potion_effect( POT_INVISIBILITY, beam.ench_power );
            contaminate_player( 1 + random2(2), beam.effect_known );
            beam.obvious_effect = true;
            nasty = false;
            nice  = true;
            break;

        case BEAM_TELEPORT:
            you_teleport();

            // An enemy helping you escape while in the Abyss, or an
            // enemy stabilizing a teleport that was about to happen.
            if (!mons_att_wont_attack(beam.attitude)
                && you.level_type == LEVEL_ABYSS)
            {
                xom_is_stimulated(255);
            }

            beam.obvious_effect = true;
            break;

        case BEAM_BLINK:
            random_blink(false);
            beam.obvious_effect = true;
            break;

        case BEAM_CHARM:
            potion_effect( POT_CONFUSION, beam.ench_power );
            beam.obvious_effect = true;
            break;     // enslavement - confusion?

        case BEAM_BANISH:
            if (YOU_KILL(beam.thrower))
            {
                mpr("This spell isn't strong enough to banish yourself.");
                break;
            }
            if (you.level_type == LEVEL_ABYSS)
            {
                mpr("You feel trapped.");
                break;
            }
            you.banished        = true;
            you.banished_by     = _beam_zapper(beam);
            beam.obvious_effect = true;
            break;

        case BEAM_PAIN:
            if (player_res_torment())
            {
                mpr("You are unaffected.");
                break;
            }

            mpr("Pain shoots through your body!");

            if (beam.aux_source.empty())
                beam.aux_source = "by nerve-wracking pain";

            _beam_ouch(beam.damage.roll(), beam);
            beam.obvious_effect = true;
            break;

        case BEAM_DISPEL_UNDEAD:
            if (!you.is_undead)
            {
                mpr("You are unaffected.");
                break;
            }

            mpr( "You convulse!" );

            if (beam.aux_source.empty())
                beam.aux_source = "by dispel undead";

            if (you.is_undead == US_SEMI_UNDEAD)
            {
                if (you.hunger_state == HS_ENGORGED)
                    beam.damage.size /= 2;
                else if (you.hunger_state > HS_SATIATED)
                {
                    beam.damage.size *= 2;
                    beam.damage.size /= 3;
                }
            }
            _beam_ouch(beam.damage.roll(), beam);
            beam.obvious_effect = true;
            break;

        case BEAM_DISINTEGRATION:
            mpr("You are blasted!");

            if (beam.aux_source.empty())
                beam.aux_source = "a disintegration bolt";

            _beam_ouch(beam.damage.roll(), beam);
            beam.obvious_effect = true;
            break;

        default:
            // _All_ enchantments should be enumerated here!
            mpr("Software bugs nibble your toes!");
            break;
        }

        if (nasty)
        {
            if (mons_att_wont_attack(beam.attitude))
            {
                beam.fr_hurt++;
                if (beam.beam_source == NON_MONSTER)
                {
                    // Beam from player rebounded and hit player.
                    if (!beam.aimed_at_feet)
                        xom_is_stimulated(255);
                }
                else
                {
                    // Beam from an ally or neutral.
                    xom_is_stimulated(128);
                }
            }
            else
                beam.foe_hurt++;
        }

        if (nice)
        {
            if (mons_att_wont_attack(beam.attitude))
                beam.fr_helped++;
            else
            {
                beam.foe_helped++;
                xom_is_stimulated(128);
            }
        }

        // Regardless of affect, we need to know if this is a stopper
        // or not - it seems all of the above are.
        return (_range_used_on_hit(beam));

        // END enchantment beam
    }

    // THE BEAM IS NOW GUARANTEED TO BE A NON-ENCHANTMENT WHICH HIT

    const bool engulfs = (beam.is_explosion || beam.is_big_cloud);
    mprf( "The %s %s you!",
          beam.name.c_str(), (engulfs) ? "engulfs" : "hits" );

    int hurted = 0;
    int burn_power = (beam.is_explosion) ? 5 :
                          (beam.is_beam) ? 3 : 2;

    // Roll the damage.
    hurted += beam.damage.roll();

#if DEBUG_DIAGNOSTICS
    int roll = hurted;
#endif

    int armour_damage_reduction = random2( 1 + player_AC() );
    if (beam.flavour == BEAM_ELECTRICITY)
        armour_damage_reduction /= 2;
    hurted -= armour_damage_reduction;

    // shrapnel
    if (beam.flavour == BEAM_FRAG && !player_light_armour())
    {
        hurted -= random2( 1 + player_AC() );
        hurted -= random2( 1 + player_AC() );
    }

#if DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS,
         "Player damage: rolled=%d; after AC=%d", roll, hurted );
#endif

    if (you.equip[EQ_BODY_ARMOUR] != -1)
    {
        if (!player_light_armour(false) && one_chance_in(4)
            && x_chance_in_y(item_mass(you.inv[you.equip[EQ_BODY_ARMOUR]]) + 1,
                             1000))
        {
            exercise( SK_ARMOUR, 1 );
        }
    }

    bool was_affected = false;
    int  old_hp       = you.hp;

    if (hurted < 0)
        hurted = 0;

    // If the beam is an actual missile or of the MMISSILE type (Earth magic)
    // we might bleed on the floor.
    if (!engulfs
        && (beam.flavour == BEAM_MISSILE || beam.flavour == BEAM_MMISSILE))
    {
        int blood = hurted/2; // assumes DVORP_PIERCING, factor: 0.5
        if (blood > you.hp)
            blood = you.hp;

        bleed_onto_floor(you.pos(), -1, blood, true);
    }

    hurted = check_your_resists( hurted, beam.flavour );

    if (beam.flavour == BEAM_MIASMA && hurted > 0)
    {
        if (player_res_poison() <= 0)
        {
            poison_player(1);
            was_affected = true;
        }

        if (one_chance_in(3 + 2 * player_prot_life()))
        {
            potion_effect(POT_SLOWING, 5);
            was_affected = true;
        }
    }

    // handling of missiles
    if (item && item->base_type == OBJ_MISSILES)
    {
        if (item->sub_type == MI_THROWING_NET)
        {
            player_caught_in_net();
            was_affected = true;
        }
        else if (item->special == SPMSL_POISONED)
        {
            if (!player_res_poison()
                && (hurted || beam.ench_power == AUTOMATIC_HIT
                              && x_chance_in_y(90 - 3 * player_AC(), 100)))
            {
                poison_player( 1 + random2(3) );
                was_affected = true;
            }
        }
        else if (item->special == SPMSL_CURARE)
        {
            if (x_chance_in_y(90 - 3 * player_AC(), 100))
            {
                curare_hits_player(actor_to_death_source(beam.agent()),
                                   1 + random2(3));
                was_affected = true;
            }
        }
    }

    // Sticky flame.
    if (beam.name == "sticky flame")
    {
        if (!player_res_sticky_flame())
        {
            sticky_flame_player();
            was_affected = true;
        }
    }

    if (affect_items)
    {
        // Simple cases for scroll burns.
        if (beam.flavour == BEAM_LAVA || beam.name == "hellfire")
            expose_player_to_element(BEAM_LAVA, burn_power);
        
        // More complex (geez..)
        if (beam.flavour == BEAM_FIRE && beam.name != "ball of steam")
            expose_player_to_element(BEAM_FIRE, burn_power);

        // Potions exploding.
        if (beam.flavour == BEAM_COLD)
            expose_player_to_element(BEAM_COLD, burn_power);
        
        if (beam.flavour == BEAM_ACID)
            splash_with_acid(5);

        // Spore pops.
        if (beam.in_explosion_phase && beam.flavour == BEAM_SPORE)
            expose_player_to_element(BEAM_SPORE, burn_power);
    }
        
#if DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS, "Damage: %d", hurted );
#endif

    if (hurted > 0 || old_hp < you.hp || was_affected)
    {
        if (mons_att_wont_attack(beam.attitude))
        {
            beam.fr_hurt++;

            // Beam from player rebounded and hit player.
            // Xom's amusement at the player's being damaged is handled
            // elsewhere.
            if (beam.beam_source == NON_MONSTER)
            {
                if (!beam.aimed_at_feet)
                    xom_is_stimulated(255);
            }
            else if (was_affected)
                xom_is_stimulated(128);
        }
        else
            beam.foe_hurt++;
    }

    _beam_ouch(hurted, beam);

    return (_range_used_on_hit( beam ));
}

static int _beam_source(const bolt &beam)
{
    return (MON_KILL(beam.thrower)     ? beam.beam_source :
            beam.thrower == KILL_MISC  ? MHITNOT
                                       : MHITYOU);
}

static int _name_to_skill_level(const std::string& name)
{
    skill_type type = SK_THROWING;

    if (name.find("dart") != std::string::npos)
        type = SK_DARTS;
    else if (name.find("needle") != std::string::npos)
        type = SK_DARTS;
    else if (name.find("bolt") != std::string::npos)
        type = SK_CROSSBOWS;
    else if (name.find("arrow") != std::string::npos)
        type = SK_BOWS;
    else if (name.find("stone") != std::string::npos)
        type = SK_SLINGS;

    if (type == SK_DARTS || type == SK_SLINGS)
        return (you.skills[type] + you.skills[SK_THROWING]);

    return (2 * you.skills[type]);
}

static void _update_hurt_or_helped(bolt &beam, monsters *mon)
{
    if (!mons_atts_aligned(beam.attitude, mons_attitude(mon)))
    {
        if (_nasty_beam(mon, beam))
            beam.foe_hurt++;
        else if (_nice_beam(mon, beam))
            beam.foe_helped++;
    }
    else
    {
        if (_nasty_beam(mon, beam))
        {
            beam.fr_hurt++;

            // Harmful beam from this monster rebounded and hit the monster.
            int midx = monster_index(mon);
            if (midx == beam.beam_source)
                xom_is_stimulated(128);
        }
        else if (_nice_beam(mon, beam))
            beam.fr_helped++;
    }
}

// Returns amount of range used up by affectation of this monster.
static int _affect_monster(bolt &beam, monsters *mon, item_def *item)
{
    const int tid = mgrd(mon->pos());
    const int mons_type  = menv[tid].type;
    const int thrower    = YOU_KILL(beam.thrower) ? KILL_YOU_MISSILE
                                                  : KILL_MON_MISSILE;
    const bool submerged = mon->submerged();

    int hurt;
    int hurt_final;

    // digging -- don't care.
    if (beam.flavour == BEAM_DIGGING)
        return (0);

    // fire storm creates these, so we'll avoid affecting them
    if (beam.name == "great blast of fire"
        && mon->type == MONS_FIRE_VORTEX)
    {
        return (0);
    }

    // check for tracer
    if (beam.is_tracer)
    {
        // Can we see this monster?
        if (!beam.can_see_invis && menv[tid].invisible()
            || (thrower == KILL_YOU_MISSILE && !see_grid(mon->pos())))
        {
            // Can't see this monster, ignore it.
            return 0;
        }

        // Is this a self-detonating monster? Don't consider it either way
        // if it is.
        if (mons_self_destructs(mon))
            return (BEAM_STOP);

        if (!mons_atts_aligned(beam.attitude, mons_attitude(mon)))
        {
            beam.foe_count += 1;
            beam.foe_power += mons_power(mons_type);
        }
        else
        {
            beam.fr_count += 1;
            beam.fr_power += mons_power(mons_type);
        }
    }
    else if ((beam.flavour == BEAM_DISINTEGRATION || beam.flavour == BEAM_NUKE)
             && mons_is_statue(mons_type) && !mons_is_icy(mons_type))
    {
        if (!silenced(you.pos()))
        {
            if (!see_grid( mon->pos() ))
                mpr("You hear a hideous screaming!", MSGCH_SOUND);
            else
            {
                mpr("The statue screams as its substance crumbles away!",
                    MSGCH_SOUND);
            }
        }
        else if (see_grid( mon->pos() ))
        {
                mpr("The statue twists and shakes as its substance "
                    "crumbles away!");
        }
        beam.obvious_effect = true;
        _update_hurt_or_helped(beam, mon);
        mon->hurt(beam.agent(), INSTANT_DEATH);
        return (BEAM_STOP);
    }

    bool hit_woke_orc = false;
    if (beam.is_enchantment()) // enchantments: no to-hit check
    {
        if (beam.is_tracer)
        {
            if (beam.thrower == KILL_YOU_MISSILE || beam.thrower == KILL_YOU)
            {
                if (!_beam_is_harmless(beam, mon)
                    && (beam.fr_count == 1 && !beam.dont_stop_fr
                        || beam.foe_count == 1 && !beam.dont_stop_foe))
                {
                    const bool target = (beam.target == mon->pos());

                    if (stop_attack_prompt(mon, true, target))
                    {
                        beam.beam_cancelled = true;
                        return (BEAM_STOP);
                    }
                    if (beam.fr_count == 1 && !beam.dont_stop_fr)
                        beam.dont_stop_fr = true;
                    else
                        beam.dont_stop_foe = true;
                }
            }

            return (_range_used_on_hit(beam));
        }

        // BEGIN non-tracer enchantment

        // Submerged monsters are unaffected by enchantments.
        if (submerged)
            return (0);

        god_conduct_trigger conducts[3];
        disable_attack_conducts(conducts);

        // Nasty enchantments will annoy the monster, and are considered
        // naughty (even if a monster might resist).
        if (_nasty_beam(mon, beam))
        {
            if (YOU_KILL(beam.thrower))
            {
                if (is_sanctuary(mon->pos()) || is_sanctuary(you.pos()))
                    remove_sanctuary(true);

                set_attack_conducts(conducts, mon, player_monster_visible(mon));

                if (you.religion == GOD_BEOGH
                    && mons_species(mon->type) == MONS_ORC
                    && mons_is_sleeping(mon) && !player_under_penance()
                    && you.piety >= piety_breakpoint(2) && mons_near(mon))
                {
                    hit_woke_orc = true;
                }
            }

            behaviour_event(mon, ME_ANNOY, _beam_source(beam));
        }
        else
            behaviour_event(mon, ME_ALERT, _beam_source(beam));

        enable_attack_conducts(conducts);

        // Doing this here so that the player gets to see monsters
        // "flicker and vanish" when turning invisible....
        _ench_animation( beam.flavour, mon );

        // now do enchantment affect
        mon_resist_type ench_result = _affect_monster_enchantment(beam, mon);
        if (mon->alive())
        {
            if (mons_is_mimic(mon->type))
                mimic_alert(mon);

            switch (ench_result)
            {
            case MON_RESIST:
                if (simple_monster_message(mon, " resists."))
                    beam.msg_generated = true;
                break;
            case MON_UNAFFECTED:
                if (simple_monster_message(mon, " is unaffected."))
                    beam.msg_generated = true;
                break;
            default:
                _update_hurt_or_helped(beam, mon);
                break;
            }
        }
        if (hit_woke_orc)
            beogh_follower_convert(mon, true);

        return (_range_used_on_hit(beam));

        // END non-tracer enchantment
    }


    // BEGIN non-enchantment (could still be tracer)
    if (submerged && !beam.aimed_at_spot)
        return (0);                   // missed me!

    // We need to know how much the monster _would_ be hurt by this, before
    // we decide if it actually hits.

    // Roll the damage:
    hurt = beam.damage.roll();

    // Water absorbs some of the damage for submerged monsters.
    if (submerged)
    {
        // Can't hurt submerged water creatures with electricity.
        if (beam.flavour == BEAM_ELECTRICITY)
        {
            if (see_grid(mon->pos()) && !beam.is_tracer)
            {
                mprf("The %s arcs harmlessly into the water.",
                     beam.name.c_str());
            }
            return (BEAM_STOP);
        }

        hurt = hurt * 2 / 3;
    }

    hurt_final = hurt;

    if (beam.is_tracer)
        hurt_final -= std::max(mon->ac / 2, 0);
    else
        hurt_final -= random2(1 + mon->ac);

    if (beam.flavour == BEAM_FRAG)
    {
        hurt_final -= random2(1 + mon->ac);
        hurt_final -= random2(1 + mon->ac);
    }

    if (hurt_final < 0)
        hurt_final = 0;

    const int raw_damage = hurt_final;

    // Check monster resists, _without_ side effects (since the
    // beam/missile might yet miss!)
    hurt_final = mons_adjust_flavoured( mon, beam, raw_damage, false );

#if DEBUG_DIAGNOSTICS
    if (!beam.is_tracer)
    {
        mprf(MSGCH_DIAGNOSTICS,
             "Monster: %s; Damage: pre-AC: %d; post-AC: %d; post-resist: %d",
             mon->name(DESC_PLAIN).c_str(), hurt, raw_damage, hurt_final);
    }
#endif

    // Now, we know how much this monster would (probably) be
    // hurt by this beam.
    if (beam.is_tracer)
    {
        if (beam.thrower == KILL_YOU_MISSILE || beam.thrower == KILL_YOU)
        {
            if (!_beam_is_harmless(beam, mon)
                && (beam.fr_count == 1 && !beam.dont_stop_fr
                    || beam.foe_count == 1 && !beam.dont_stop_foe))
            {
                const bool target = (beam.target == mon->pos());

                if (stop_attack_prompt(mon, true, target))
                {
                    beam.beam_cancelled = true;
                    return (BEAM_STOP);
                }
                if (beam.fr_count == 1 && !beam.dont_stop_fr)
                    beam.dont_stop_fr = true;
                else
                    beam.dont_stop_foe = true;
            }
        }

        // Check only if actual damage.
        if (hurt_final > 0 && hurt > 0)
        {
            // Monster could be hurt somewhat, but only apply the
            // monster's power based on how badly it is affected.
            // For example, if a fire giant (power 16) threw a
            // fireball at another fire giant, and it only took
            // 1/3 damage, then power of 5 would be applied to
            // foe_power or fr_power.
            if (!mons_atts_aligned(beam.attitude, mons_attitude(mon)))
            {
                // Counting foes is only important for monster tracers.
                beam.foe_power += 2 * hurt_final * mons_power(mons_type)
                                                    / hurt;
            }
            else
            {
                beam.fr_power += 2 * hurt_final * mons_power(mons_type)
                                                   / hurt;
            }
        }

        // Either way, we could hit this monster, so return range used.
        return (_range_used_on_hit(beam));
    }
    // END non-enchantment (could still be tracer)

    // BEGIN real non-enchantment beam

    // Player beams which hit friendlies or good neutrals will annoy
    // them and be considered naughty if they do damage (this is so as
    // not to penalize players that fling fireballs into a melee with
    // fire elementals on their side - the elementals won't give a sh*t,
    // after all).

    god_conduct_trigger conducts[3];
    disable_attack_conducts(conducts);

    if (_nasty_beam(mon, beam))
    {
        if (YOU_KILL(beam.thrower) && hurt_final > 0)
        {
            // It's not the player's fault if he didn't see the monster or
            // the monster was caught in an unexpected blast of ?immolation.
            const bool okay =
                (!player_monster_visible(mon)
                 || beam.aux_source == "reading a scroll of immolation"
                    && !beam.effect_known);

            if (is_sanctuary(mon->pos()) || is_sanctuary(you.pos()))
                remove_sanctuary(true);

            set_attack_conducts(conducts, mon, !okay);
        }

        if (you.religion == GOD_BEOGH && mons_species(mon->type) == MONS_ORC
            && mons_is_sleeping(mon) && YOU_KILL(beam.thrower)
            && !player_under_penance() && you.piety >= piety_breakpoint(2)
            && mons_near(mon))
        {
            hit_woke_orc = true;
        }
    }

    // Explosions always 'hit'.
    const bool engulfs = (beam.is_explosion || beam.is_big_cloud);

    int beam_hit = beam.hit;
    if (menv[tid].invisible() && !beam.can_see_invis)
        beam_hit /= 2;

    // FIXME We're randomising mon->evasion, which is further
    // randomised inside test_beam_hit. This is so we stay close to the 4.0
    // to-hit system (which had very little love for monsters).
    if (!engulfs && !test_beam_hit(beam_hit, random2(mon->ev)))
    {
        // If the PLAYER cannot see the monster, don't tell them anything!
        if (player_monster_visible( &menv[tid] ) && mons_near(mon))
        {
            msg::stream << "The " << beam.name << " misses "
                        << mon->name(DESC_NOCAP_THE) << '.' << std::endl;
        }
        return (0);
    }

    // The monster may block the beam.
    if (!engulfs && _beam_is_blockable(beam))
    {
        const int shield_block = mon->shield_bonus();
        if (shield_block > 0)
        {
            const int hit = random2( beam.hit * 130 / 100
                                     + mon->shield_block_penalty() );
            if (hit < shield_block && mons_near(mon)
                && player_monster_visible(mon))
            {
                mprf("%s blocks the %s.",
                     mon->name(DESC_CAP_THE).c_str(),
                     beam.name.c_str());

                mon->shield_block_succeeded();
                return (BEAM_STOP);
            }
        }
    }

    _update_hurt_or_helped(beam, mon);

    enable_attack_conducts(conducts);

    // The beam hit.
    if (mons_near(mon))
    {
        mprf("The %s %s %s.",
             beam.name.c_str(),
             engulfs? "engulfs" : "hits",
             player_monster_visible(&menv[tid]) ?
                 mon->name(DESC_NOCAP_THE).c_str() : "something");

    }
    else
    {
        // The player might hear something, if _they_ fired a missile
        // (not beam).
        if (!silenced(you.pos()) && beam.flavour == BEAM_MISSILE
            && YOU_KILL(beam.thrower))
        {
            mprf(MSGCH_SOUND, "The %s hits something.", beam.name.c_str());
        }
    }

    // handling of missiles
    if (item && item->base_type == OBJ_MISSILES
        && item->sub_type == MI_THROWING_NET)
    {
        monster_caught_in_net(mon, beam);
    }

    // Note that hurt_final was calculated above, so we don't need it again.
    // just need to apply flavoured specials (since we called with
    // doFlavouredEffects = false above).
    hurt_final = mons_adjust_flavoured(mon, beam, raw_damage);

    // If the beam is an actual missile or of the MMISSILE type (Earth magic)
    // we might bleed on the floor.
    if (!engulfs
        && (beam.flavour == BEAM_MISSILE || beam.flavour == BEAM_MMISSILE)
        && !mons_is_summoned(mon) && !mons_is_submerged(mon))
    {
        // Using raw_damage instead of the flavoured one!
        int blood = raw_damage/2; // assumes DVORP_PIERCING, factor: 0.5
        if (blood > mon->hit_points)
            blood = mon->hit_points;

        bleed_onto_floor(mon->pos(), mon->type, blood, true);
    }

    // Now hurt monster.
    mon->hurt(beam.agent(), hurt_final, beam.flavour);

    if (mon->alive())
    {
        if (thrower == KILL_YOU_MISSILE && mons_near(mon))
            print_wounds(mon);

        // Don't annoy friendlies or good neutrals if the player's beam
        // did no damage.  Hostiles will still take umbrage.
        if (hurt_final > 0 || !mons_wont_attack(mon) || !YOU_KILL(beam.thrower))
            behaviour_event(mon, ME_ANNOY, _beam_source(beam));

        // sticky flame
        if (beam.name == "sticky flame")
        {
            int levels = std::min(4, 1 + random2(hurt_final) / 2);

            sticky_flame_monster(tid, _whose_kill(beam), levels);
        }

        // Handle missile effects.
        if (item)
        {
            if (item->base_type == OBJ_MISSILES
                && item->special == SPMSL_POISONED)
            {
                int num_levels = 0;
                // ench_power == AUTOMATIC_HIT if this is a poisoned needle.
                if (beam.ench_power == AUTOMATIC_HIT
                    && x_chance_in_y(90 - 3 * mon->ac, 100))
                {
                    num_levels = 2;
                }
                else if (random2(hurt_final) - random2(mon->ac) > 0)
                    num_levels = 1;

                int num_success = 0;
                if (YOU_KILL(beam.thrower))
                {
                    const int skill_level = _name_to_skill_level(beam.name);
                    if (x_chance_in_y(skill_level + 25, 50))
                        num_success++;
                    if (x_chance_in_y(skill_level, 50))
                        num_success++;
                }
                else
                    num_success = 1;

                if (num_success)
                {
                    if (num_success == 2)
                        num_levels++;
                    poison_monster( mon, _whose_kill(beam), num_levels );
                }
            }
        }

        bool wake_mimic = true;
        if (item && item->base_type == OBJ_MISSILES
            && item->special == SPMSL_CURARE)
        {
            if (beam.ench_power == AUTOMATIC_HIT
                && curare_hits_monster( beam, mon, _whose_kill(beam), 2 ))
            {
                wake_mimic = false;
            }
        }

        if (wake_mimic && mons_is_mimic( mon->type ))
            mimic_alert(mon);
        else if (hit_woke_orc)
            beogh_follower_convert(mon, true);
    }

    return (_range_used_on_hit(beam));
}

bool _beam_has_saving_throw(const bolt& beam)
{
    if (beam.aimed_at_feet)
        return (false);

    bool rc = true;

    switch (beam.flavour)
    {
    case BEAM_HASTE:
    case BEAM_HEALING:
    case BEAM_INVISIBILITY:
    case BEAM_DISPEL_UNDEAD:
    case BEAM_ENSLAVE_DEMON:    // it has a different saving throw
        rc = false;
        break;
    default:
        break;
    }
    return rc;
}

bool _ench_flavour_affects_monster(beam_type flavour, const monsters* mon)
{
    bool rc = true;
    switch (flavour)
    {
    case BEAM_POLYMORPH:
        rc = mon->can_mutate();
        break;

    case BEAM_DEGENERATE:
        rc = (mons_holiness(mon) == MH_NATURAL
              && mon->type != MONS_PULSATING_LUMP);
        break;

    case BEAM_ENSLAVE_UNDEAD:
        rc = (mons_holiness(mon) == MH_UNDEAD && mon->attitude != ATT_FRIENDLY);
        break;

    case BEAM_DISPEL_UNDEAD:
        rc = (mons_holiness(mon) == MH_UNDEAD);
        break;

    case BEAM_ENSLAVE_DEMON:
        rc = (mons_holiness(mon) == MH_DEMONIC && !mons_friendly(mon));
        break;

    case BEAM_PAIN:
        rc = !mons_res_negative_energy(mon);
        break;

    case BEAM_SLEEP:
        rc = !mon->has_ench(ENCH_SLEEP_WARY)     // slept recently
            && mons_holiness(mon) == MH_NATURAL  // no unnatural
            && mons_res_cold(mon) <= 0;          // can't be hibernated
        break;

    default:
        break;
    }

    return rc;
}

static mon_resist_type _affect_monster_enchantment(bolt &beam, monsters *mon)
{
    // Early out if the enchantment is meaningless.
    if (!_ench_flavour_affects_monster(beam.flavour, mon))
        return (MON_UNAFFECTED);

    // Check magic resistance.
    if (_beam_has_saving_throw(beam))
    {
        if (mons_immune_magic(mon))
            return (MON_UNAFFECTED);
        if (check_mons_resist_magic(mon, beam.ench_power))
            return (MON_RESIST);
    }

    switch (beam.flavour)
    {
    case BEAM_TELEPORT:
        if (you.can_see(mon))
            beam.obvious_effect = true;
        monster_teleport(mon, false);
        return (MON_AFFECTED);

    case BEAM_BLINK:
        if (you.can_see(mon))
            beam.obvious_effect = true;
        monster_blink(mon);
        return (MON_AFFECTED);

    case BEAM_POLYMORPH:
        if (mon->mutate())
            beam.obvious_effect = true;
        if (YOU_KILL(beam.thrower))
        {
            did_god_conduct(DID_DELIBERATE_MUTATING, 2 + random2(3),
                            beam.effect_known);
        }
        return (MON_AFFECTED);

    case BEAM_BANISH:
        if (you.level_type == LEVEL_ABYSS)
            simple_monster_message(mon, " wobbles for a moment.");
        else
            mon->banish();
        beam.obvious_effect = true;
        return (MON_AFFECTED);

    case BEAM_DEGENERATE:
        if (monster_polymorph(mon, MONS_PULSATING_LUMP))
            beam.obvious_effect = true;
        return (MON_AFFECTED);

    case BEAM_DISPEL_UNDEAD:
        if (simple_monster_message(mon, " convulses!"))
            beam.obvious_effect = true;
        mon->hurt(beam.agent(), beam.damage.roll());
        return (MON_AFFECTED);

    case BEAM_ENSLAVE_UNDEAD:
    {
        const god_type god =
            (crawl_state.is_god_acting()) ? crawl_state.which_god_acting()
                                          : GOD_NO_GOD;

#if DEBUG_DIAGNOSTICS
        mprf(MSGCH_DIAGNOSTICS,
             "HD: %d; pow: %d", mon->hit_dice, beam.ench_power);
#endif

        beam.obvious_effect = true;
        if (player_will_anger_monster(mon))
        {
            simple_monster_message(mon, " is repulsed!");
            return (MON_OTHER);
        }

        simple_monster_message(mon, " is enslaved.");

        // Wow, permanent enslaving!
        mon->attitude = ATT_FRIENDLY;
        behaviour_event(mon, ME_ALERT, MHITNOT);

        mons_make_god_gift(mon, god);

        return (MON_AFFECTED);
    }

    case BEAM_ENSLAVE_DEMON:
#if DEBUG_DIAGNOSTICS
        mprf(MSGCH_DIAGNOSTICS,
             "HD: %d; pow: %d", mon->hit_dice, beam.ench_power);
#endif

        if (mon->hit_dice * 11 / 2 >= random2(beam.ench_power)
            || mons_is_unique(mon->type))
        {
            return (MON_RESIST);
        }

        beam.obvious_effect = true;
        if (player_will_anger_monster(mon))
        {
            simple_monster_message(mon, " is repulsed!");
            return (MON_OTHER);
        }

        simple_monster_message(mon, " is enslaved.");

        // Wow, permanent enslaving! (sometimes)
        if (one_chance_in(2 + mon->hit_dice / 4))
            mon->attitude = ATT_FRIENDLY;
        else
            mon->add_ench(ENCH_CHARM);
        behaviour_event(mon, ME_ALERT, MHITNOT);
        return (MON_AFFECTED);

    case BEAM_PAIN:             // pain/agony
        if (simple_monster_message(mon, " convulses in agony!"))
            beam.obvious_effect = true;

        if (beam.name.find("agony") != std::string::npos) // agony
            mon->hit_points = std::max(mon->hit_points/2, 1);
        else                    // pain
            mon->hurt(beam.agent(), beam.damage.roll(), beam.flavour);
        return (MON_AFFECTED);

    case BEAM_DISINTEGRATION:   // disrupt/disintegrate
        if (simple_monster_message(mon, " is blasted."))
            beam.obvious_effect = true;
        mon->hurt(beam.agent(), beam.damage.roll(), beam.flavour);
        return (MON_AFFECTED);

    case BEAM_SLEEP:
        if (simple_monster_message(mon, " looks drowsy..."))
            beam.obvious_effect = true;
        mon->put_to_sleep();
        return (MON_AFFECTED);

    case BEAM_BACKLIGHT:
        if (backlight_monsters(mon->pos(), beam.hit, 0))
        {
            beam.obvious_effect = true;
            return (MON_AFFECTED);
        }
        return (MON_UNAFFECTED);

    default:
        return (mons_ench_f2(mon, beam));
    }
}


// Extra range used on hit.
static int _range_used_on_hit(bolt &beam)
{
    // Non-beams can only affect one thing (player/monster).
    if (!beam.is_beam)
        return (BEAM_STOP);

    if (beam.is_enchantment())
        return (beam.flavour == BEAM_DIGGING ? 0 : BEAM_STOP);

    // Hellfire stops for nobody!
    if (beam.name == "hellfire")
        return (0);

    // Generic explosion.
    if (beam.is_explosion || beam.is_big_cloud)
        return (BEAM_STOP);

    // Plant spit.
    if (beam.flavour == BEAM_ACID)
        return (BEAM_STOP);

    // Lava doesn't go far, but it goes through most stuff.
    if (beam.flavour == BEAM_LAVA)
        return (1);

    // If it isn't lightning, reduce range by a lot.
    if (beam.flavour != BEAM_ELECTRICITY)
        return (2);

    return (0);
}

// Takes a bolt and refines it for use in the explosion function. Called
// from missile() and beam() in beam.cc. Explosions which do not follow from
// beams (eg scrolls of immolation) bypass this function.
static void _explosion1(bolt &pbolt)
{
    int ex_size = 1;
    // convenience
    coord_def p = pbolt.target;
    const char *seeMsg  = NULL;
    const char *hearMsg = NULL;

    // Assume that the player can see/hear the explosion, or
    // gets burned by it anyway.  :)
    pbolt.msg_generated = true;

    if (pbolt.name == "hellfire")
    {
        seeMsg  = "The hellfire explodes!";
        hearMsg = "You hear a strangely unpleasant explosion.";

        pbolt.type    = dchar_glyph(DCHAR_FIRED_BURST);
        pbolt.flavour = BEAM_HELLFIRE;
    }

    if (pbolt.name == "golden flame")
    {
        seeMsg  = "The flame explodes!";
        hearMsg = "You feel a deep, resonant explosion.";

        pbolt.type    = dchar_glyph(DCHAR_FIRED_BURST);
        pbolt.flavour = BEAM_HOLY;
        ex_size = 2;
    }

    if (pbolt.name == "fireball")
    {
        seeMsg  = "The fireball explodes!";
        hearMsg = "You hear an explosion.";

        pbolt.type    = dchar_glyph(DCHAR_FIRED_BURST);
        pbolt.flavour = BEAM_FIRE;
        ex_size = 1;
    }

    if (pbolt.name == "orb of electricity")
    {
        seeMsg  = "The orb of electricity explodes!";
        hearMsg = "You hear a clap of thunder!";

        pbolt.type       = dchar_glyph(DCHAR_FIRED_BURST);
        pbolt.flavour    = BEAM_ELECTRICITY;
        pbolt.colour     = LIGHTCYAN;
        pbolt.damage.num = 1;
        ex_size          = 2;
    }

    if (pbolt.name == "orb of energy")
    {
        seeMsg  = "The orb of energy explodes.";
        hearMsg = "You hear an explosion.";
    }

    if (pbolt.name == "metal orb")
    {
        seeMsg  = "The orb explodes into a blast of deadly shrapnel!";
        hearMsg = "You hear an explosion!";

        pbolt.name    = "blast of shrapnel";
        pbolt.type    = dchar_glyph(DCHAR_FIRED_ZAP);
        pbolt.flavour = BEAM_FRAG;     // Sets it from pure damage to shrapnel
                                       // (which is absorbed extra by armour).
    }

    if (pbolt.name == "great blast of cold")
    {
        seeMsg  = "The blast explodes into a great storm of ice!";
        hearMsg = "You hear a raging storm!";

        pbolt.name       = "ice storm";
        pbolt.type       = dchar_glyph(DCHAR_FIRED_ZAP);
        pbolt.colour     = WHITE;
        ex_size          = 2 + (random2( pbolt.ench_power ) > 75);
    }

    if (pbolt.name == "ball of vapour")
    {
        seeMsg     = "The ball expands into a vile cloud!";
        hearMsg    = "You hear a gentle \'poof\'.";
        if (!pbolt.is_tracer)
            pbolt.name = "stinking cloud";
    }

    if (pbolt.name == "potion")
    {
        seeMsg     = "The potion explodes!";
        hearMsg    = "You hear an explosion!";
        if (!pbolt.is_tracer)
            pbolt.name = "cloud";
    }

    if (seeMsg == NULL)
    {
        seeMsg  = "The beam explodes into a cloud of software bugs!";
        hearMsg = "You hear the sound of one hand clapping!";
    }


    if (!pbolt.is_tracer && *seeMsg && *hearMsg)
    {
        // Check for see/hear/no msg.
        if (see_grid(p) || p == you.pos())
            mpr(seeMsg);
        else
        {
            if (!player_can_hear(p))
                pbolt.msg_generated = false;
            else
                mpr(hearMsg, MSGCH_SOUND);
        }
    }

    pbolt.ex_size = ex_size;
    explosion( pbolt );
}                               // end explosion1()


#define MAX_EXPLOSION_RADIUS 9

// explosion() is considered to emanate from beam->target_x, target_y
// and has a radius equal to ex_size.  The explosion will respect
// boundaries like walls, but go through/around statues/idols/etc.
//
// For each cell affected by the explosion, affect() is called.
int explosion( bolt &beam, bool hole_in_the_middle,
               bool explode_in_wall, bool stop_at_statues,
               bool stop_at_walls, bool show_more, bool affect_items)
{
    if (in_bounds(beam.source) && beam.source != beam.target
        && (!explode_in_wall || stop_at_statues || stop_at_walls))
    {
        ray_def ray;
        int     max_dist = grid_distance(beam.source, beam.target);

        ray.fullray_idx = -1; // to quiet valgrind
        find_ray( beam.source, beam.target, true, ray, 0, true );

        // Can cast explosions out from statues or walls.
        if (ray.pos() == beam.source)
        {
            max_dist--;
            ray.advance(true);
        }

        int dist = 0;
        while (dist++ <= max_dist && ray.pos() != beam.target)
        {
            if (grid_is_solid(ray.pos()))
            {
                bool is_wall = grid_is_wall(grd(ray.pos()));
                if (!stop_at_statues && !is_wall)
                {
#if DEBUG_DIAGNOSTICS
                    mpr("Explosion beam passing over a statue or other "
                        "non-wall solid feature.", MSGCH_DIAGNOSTICS);
#endif
                    continue;
                }
                else if (!stop_at_walls && is_wall)
                {
#if DEBUG_DIAGNOSTICS
                    mpr("Explosion beam passing through a wall.",
                        MSGCH_DIAGNOSTICS);
#endif
                    continue;
                }

#if DEBUG_DIAGNOSTICS
                if (!is_wall && stop_at_statues)
                {
                    mpr("Explosion beam stopped by a statue or other "
                        "non-wall solid feature.", MSGCH_DIAGNOSTICS);
                }
                else if (is_wall && stop_at_walls)
                {
                    mpr("Explosion beam stopped by a by wall.",
                        MSGCH_DIAGNOSTICS);
                }
                else
                {
                    mpr("Explosion beam stopped by someting buggy.",
                        MSGCH_DIAGNOSTICS);
                }
#endif

                break;
            }
            ray.advance(true);
        } // while (dist++ <= max_dist)

        // Backup so we don't explode inside the wall.
        if (!explode_in_wall && grid_is_wall(grd(ray.pos())))
        {
#if DEBUG_DIAGNOSTICS
            int old_x = ray.x();
            int old_y = ray.y();
#endif
            ray.regress();
#if DEBUG_DIAGNOSTICS
            mprf(MSGCH_DIAGNOSTICS,
                 "Can't explode in a solid wall, backing up a step "
                 "along the beam path from (%d, %d) to (%d, %d)",
                 old_x, old_y, ray.x(), ray.y());
#endif
        }
        beam.target = ray.pos();
    } // if (!explode_in_wall)

    int r = beam.ex_size;

    // Beam is now an explosion.
    beam.in_explosion_phase = true;

    if (is_sanctuary(beam.target))
    {
        if (!beam.is_tracer && see_grid(beam.target) && !beam.name.empty())
        {
            mprf(MSGCH_GOD, "By Zin's power, the %s is contained.",
                 beam.name.c_str());
        }
        return (-1);
    }

#if DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS,
         "explosion at (%d, %d) : t=%d c=%d f=%d hit=%d dam=%dd%d",
         beam.target.x, beam.target.y,
         beam.type, beam.colour, beam.flavour,
         beam.hit, beam.damage.num, beam.damage.size );
#endif

    // For now, we don't support explosions greater than 9 radius.
    if (r > MAX_EXPLOSION_RADIUS)
        r = MAX_EXPLOSION_RADIUS;

    // make a noise
    noisy(10 + 5 * r, beam.target);

    // set map to false
    explode_map.init(false);

    // Discover affected cells - recursion is your friend!
    // this is done to model an explosion's behaviour around
    // corners where a simple 'line of sight' isn't quite
    // enough.  This might be slow for really big explosions,
    // as the recursion runs approximately as R^2.
    _explosion_map(beam, coord_def(0, 0), 0, 0, r);

    // Go through affected cells, drawing effect and
    // calling affect() for each.  Now, we get a bit
    // fancy, drawing all radius 0 effects, then radius 1,
    // radius 2, etc.  It looks a bit better that way.

    // turn buffering off
#ifdef WIN32CONSOLE
    bool oldValue = true;
    if (!beam.is_tracer)
        oldValue = set_buffering(false);
#endif

    // --------------------- begin boom ---------------

    bool drawing = true;
    for (int i = 0; i < 2; i++)
    {
        // do center -- but only if its affected
        if (!hole_in_the_middle)
            _explosion_cell(beam, coord_def(0, 0), drawing, affect_items);

        // do the rest of it
        for (int rad = 1; rad <= r; rad ++)
        {
            // do sides
            for (int ay = 1 - rad; ay <= rad - 1; ay += 1)
            {
                if (explode_map[-rad+9][ay+9])
                {
                    _explosion_cell(beam, coord_def(-rad, ay), drawing,
                                    affect_items);
                }
                if (explode_map[rad+9][ay+9])
                {
                    _explosion_cell(beam, coord_def(rad, ay), drawing,
                                    affect_items);
                }
            }

            // do top & bottom
            for (int ax = -rad; ax <= rad; ax += 1)
            {
                if (explode_map[ax+9][-rad+9])
                {
                    _explosion_cell(beam, coord_def(ax, -rad), drawing,
                                    affect_items);
                }
                if (explode_map[ax+9][rad+9])
                {
                    _explosion_cell(beam, coord_def(ax, rad), drawing,
                                    affect_items);
                }
            }

            // new-- delay after every 'ring' {gdl}
            // If we don't refresh curses we won't
            // guarantee that the explosion is visible.
            if (drawing)
                update_screen();
            // Only delay on real explosion.
            if (!beam.is_tracer && drawing)
                delay(50);
        }

        drawing = false;
    }

    int cells_seen = 0;
    for ( int i = -9; i <= 9; ++i )
        for ( int j = -9; j <= 9; ++j )
            if ( explode_map[i+9][j+9]
                 && see_grid(beam.target + coord_def(i,j)))
            {
                cells_seen++;
            }

    // ---------------- end boom --------------------------

#ifdef WIN32CONSOLE
    if (!beam.is_tracer)
        set_buffering(oldValue);
#endif

    // Duplicate old behaviour - pause after entire explosion
    // has been drawn.
    if (!beam.is_tracer && cells_seen > 0 && show_more)
        more();

    return (cells_seen);
}

static void _explosion_cell(bolt &beam, const coord_def& p, bool drawOnly,
                            bool affect_items)
{
    bool random_beam = false;
    coord_def realpos = beam.target + p;

    if (!drawOnly)
    {
        // Random beams: randomize before affect().
        if (beam.flavour == BEAM_RANDOM)
        {
            random_beam  = true;
            beam.flavour = static_cast<beam_type>(
                               random_range(BEAM_FIRE, BEAM_ACID) );
        }

        affect(beam, realpos, NULL, affect_items);

        if (random_beam)
            beam.flavour = BEAM_RANDOM;
    }

    // Early out for tracer.
    if (beam.is_tracer)
        return;

    if (drawOnly)
    {
        const coord_def drawpos = grid2view(realpos);

        // XXX Don't you always see your own grid?
        if (see_grid(realpos) || realpos == you.pos())
        {
#ifdef USE_TILE
            if (in_los_bounds(drawpos))
                tiles.add_overlay(realpos, tileidx_bolt(beam));
#else
            // bounds check
            if (in_los_bounds(drawpos))
            {
                cgotoxy(drawpos.x, drawpos.y, GOTO_DNGN);
                put_colour_ch(
                    beam.colour == BLACK ? random_colour() : beam.colour,
                    dchar_glyph( DCHAR_EXPLOSION ) );
            }
#endif
        }
    }
}

static void _explosion_map( bolt &beam, const coord_def& p,
                           int count, int dir, int r )
{
    // Check to see out of range.
    if (p.abs() > r*(r+1))
        return;

    // Check count.
    if (count > 10*r)
        return;

    const coord_def loc(beam.target + p);

    // Make sure we haven't run off the map.
    if (!map_bounds(loc))
        return;

    // Check sanctuary.
    if (is_sanctuary(loc))
        return;

    const dungeon_feature_type dngn_feat = grd(loc);

    // Check to see if we're blocked by something specifically, we're
    // blocked by WALLS.  Not statues, idols, etc.  Special case:
    // Explosion originates from rock/statue (e.g. Lee's rapid
    // deconstruction) - in this case, ignore solid cells at the
    // center of the explosion.

    if (grid_is_wall(dngn_feat)
        || dngn_feat == DNGN_SECRET_DOOR
        || dngn_feat == DNGN_CLOSED_DOOR)
    {
        if (!(_affects_wall(beam, dngn_feat) && p.origin()))
            return;
    }

    // Hmm, I think we're ok.
    explode_map(p + coord_def(9,9)) = true;

    const coord_def spread[] = { coord_def(0, -1), coord_def( 0, 1),
                                 coord_def(1,  0), coord_def(-1, 0) };
    const int opdir[]        = { 2, 1, 4, 3 };

    // Now recurse in every direction except the one we came from.
    const int spreadsize = ARRAYSZ(spread);
    for (int i = 0; i < spreadsize; i++)
    {
        if (i+1 != dir)
        {
            int cadd = 5;
            if (p.x * spread[i].x < 0 || p.y * spread[i].y < 0)
                cadd = 17;

            _explosion_map( beam, p + spread[i], count + cadd, opdir[i], r );
        }
    }
}

// Returns true if the beam is harmful (ignoring monster
// resists) -- mon is given for 'special' cases where,
// for example, "Heal" might actually hurt undead, or
// "Holy Word" being ignored by holy monsters, etc.
//
// Only enchantments should need the actual monster type
// to determine this; non-enchantments are pretty
// straightforward.
static bool _nasty_beam(monsters *mon, const bolt &beam)
{
    // Take care of non-enchantments.
    if (!beam.is_enchantment())
        return (true);

    // Now for some non-hurtful enchantments.

    // No charming holy beings!
    if (beam.flavour == BEAM_CHARM)
        return (mons_is_holy(mon));

    // degeneration / sleep
    if (beam.flavour == BEAM_DEGENERATE || beam.flavour == BEAM_SLEEP)
        return (mons_holiness(mon) == MH_NATURAL);

    // dispel undead / control undead
    if (beam.flavour == BEAM_DISPEL_UNDEAD
        || beam.flavour == BEAM_ENSLAVE_UNDEAD)
    {
        return (mons_holiness(mon) == MH_UNDEAD);
    }

    // pain/agony
    if (beam.flavour == BEAM_PAIN)
        return (!mons_res_negative_energy(mon));

    // control demon
    if (beam.flavour == BEAM_ENSLAVE_DEMON)
        return (mons_holiness(mon) == MH_DEMONIC);

    // haste/healing/invisibility
    if (beam.flavour == BEAM_HASTE || beam.flavour == BEAM_HEALING
        || beam.flavour == BEAM_INVISIBILITY)
    {
        return (false);
     }

    // everything else is considered nasty by everyone
    return (true);
}

static bool _nice_beam(monsters *mon, const bolt &beam)
{
    // haste/healing/invisibility
    if (beam.flavour == BEAM_HASTE || beam.flavour == BEAM_HEALING
        || beam.flavour == BEAM_INVISIBILITY)
    {
        return (true);
    }

    return (false);
}

////////////////////////////////////////////////////////////////////////////
// bolt

// A constructor for bolt to help guarantee that we start clean (this has
// caused way too many bugs).  Putting it here since there's no good place to
// put it, and it doesn't do anything other than initialize it's members.
//
// TODO: Eventually it'd be nice to have a proper factory for these things
// (extended from setup_mons_cast() and zapping() which act as limited ones).
bolt::bolt() : range(0), type('*'),
               colour(BLACK),
               flavour(BEAM_MAGIC), source(), target(), pos(), damage(0,0),
               ench_power(0), hit(0),
               thrower(KILL_MISC), ex_size(0), beam_source(MHITNOT), name(),
               is_beam(false), is_explosion(false), is_big_cloud(false),
               aimed_at_spot(false),
               aux_source(), affects_nothing(false), obvious_effect(false),
               effect_known(true), fr_count(0), foe_count(0), fr_power(0),
               foe_power(0), fr_hurt(0), foe_hurt(0), fr_helped(0),
               foe_helped(0), is_tracer(false), aimed_at_feet(false),
               msg_generated(false), in_explosion_phase(false),
               smart_monster(false), can_see_invis(false),
               attitude(ATT_HOSTILE), foe_ratio(0), chose_ray(false),
               beam_cancelled(false), dont_stop_foe(false),
               dont_stop_fr(false), dont_stop_player(false)
{
}

killer_type bolt::killer() const
{
    if (flavour == BEAM_BANISH)
        return (KILL_RESET);

    switch (thrower)
    {
    case KILL_YOU:
    case KILL_YOU_MISSILE:
        return (flavour == BEAM_PARALYSIS
                || flavour == BEAM_PETRIFY) ? KILL_YOU : KILL_YOU_MISSILE;

    case KILL_MON:
    case KILL_MON_MISSILE:
        return (KILL_MON_MISSILE);

    case KILL_YOU_CONF:
        return (KILL_YOU_CONF);

    default:
        return (KILL_MON_MISSILE);
    }
}

void bolt::set_target(const dist &d)
{
    if (!d.isValid)
        return;

    target = d.target;

    chose_ray = d.choseRay;
    if (d.choseRay)
        ray = d.ray;

    if (d.isEndpoint)
        aimed_at_spot = true;
}

void bolt::setup_retrace()
{
    if (pos.x && pos.y)
        target = pos;

    std::swap(source, target);
    affects_nothing = true;
    aimed_at_spot   = true;
}

actor* bolt::agent() const
{
    if (YOU_KILL(this->thrower))
        return (&you);
    else if (this->beam_source != NON_MONSTER)
        return (&menv[this->beam_source]);
    else
        return (NULL);
}

bool bolt::is_enchantment() const
{
    return (this->flavour >= BEAM_FIRST_ENCHANTMENT
            && this->flavour <= BEAM_LAST_ENCHANTMENT);
}
