/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "pr_comp.h"			// defs shared with qcc
#include "progdefs.h"			// generated by program cdefs

//#define PR_DEBUGGER			// JDH: if defined, in-game QC debugger code is included (UNFINISHED)

typedef union eval_s
{
	string_t	string;
	float		_float;
	float		vector[3];
	func_t		function;
	int			_int;
	int			edict;
} eval_t;


// JDH: increased MAX_ENT_LEAFS from 16 - fixes disappearing brush models
#define	MAX_ENT_LEAFS	64
typedef struct edict_s
{
	qboolean		free;
	link_t			area;			// linked to a division node or leaf

	int				num_leafs;
	unsigned short	leafnums[MAX_ENT_LEAFS];		// JDH: was signed short

	entity_state_t	baseline;

	float			freetime;		// sv.time when the object was freed
#ifdef HEXEN2_SUPPORT
	float			alloctime;		// sv.time when the object was allocated
#endif
	entvars_t	v;			// C exported fields from progs
// other fields from progs come immediately after
} edict_t;

#define	EDICT_FROM_AREA(l) STRUCT_FROM_LINK((l), edict_t, area)
#define	GETEDICTFIELD(ed, fieldoffset) (fieldoffset ? (eval_t *)((byte *)&(ed)->v + (fieldoffset)) : NULL)
#define	GETFIELDFLOAT(ed, ofs) (((eval_t *)((byte *)&(ed)->v + (ofs)))->_float)
#define	GETFIELDINT(ed, ofs)   (((eval_t *)((byte *)&(ed)->v + (ofs)))->_int)
#define	GETFIELDEDICT(ed, ofs) (((eval_t *)((byte *)&(ed)->v + (ofs)))->edict)
#define	GETFIELDFUNC(ed, ofs)  (((eval_t *)((byte *)&(ed)->v + (ofs)))->function)

extern	int	eval_gravity, eval_items2, eval_ammo_shells1, eval_ammo_nails1;
extern	int	eval_ammo_lava_nails, eval_ammo_rockets1, eval_ammo_multi_rockets;
extern	int	eval_ammo_cells1, eval_ammo_plasma;

// nehahra specific
extern	int	eval_alpha, eval_fullbright, /*eval_idealpitch, */eval_pitch_speed;

//============================================================================

extern	dprograms_t		*progs;
extern	dfunction_t		*pr_functions;
extern	char			*pr_strings;
extern	dstatement_t	*pr_statements;
extern	float			*pr_globals;

extern	int		pr_edict_size;		// in bytes

/******JDH******/

//extern	ddef_t		*pr_globaldefs;
//extern	ddef_t		*pr_fielddefs;

// pointers to global variables in globalvars_t.  Provides a level of abstraction
//  so the structure of globalvars_t can change (eg. Quake vs. Hexen II)
typedef struct globalptrs_s
{
	int			*self;
	int			*other;
	float		*time;
	float		*frametime;
	float		*force_retouch;
	string_t	*mapname;
	float		*deathmatch;
	float		*coop;
	float		*serverflags;
	float		*total_secrets;
	float		*total_monsters;
	float		*found_secrets;
	float		*killed_monsters;
	float		*parm1;
	vec3_t		*v_forward;
	vec3_t		*v_up;
	vec3_t		*v_right;
	float		*trace_allsolid;
	float		*trace_startsolid;
	float		*trace_fraction;
	vec3_t		*trace_endpos;
	vec3_t		*trace_plane_normal;
	float		*trace_plane_dist;
	int			*trace_ent;
	float		*trace_inopen;
	float		*trace_inwater;
	int			*msg_entity;
	func_t		*StartFrame;
	func_t		*PlayerPreThink;
	func_t		*PlayerPostThink;
	func_t		*ClientKill;
	func_t		*ClientConnect;
	func_t		*PutClientInServer;
	func_t		*ClientDisconnect;
	func_t		*SetNewParms;
	func_t		*SetChangeParms;
#ifdef HEXEN2_SUPPORT
// Hexen II-specific (valid only if hexen2 is true):
	float		*cycle_wrapped;
	float		*cl_playerclass;
	string_t	*startspot;
	float		*randomclass;
	func_t		*ClassChangeWeapon;
	func_t		*ClientReEnter;
#endif
} globalptrs_t;

extern globalptrs_t pr_global_ptrs;

#define PR_GLOBAL(f) (*pr_global_ptrs.f)		// FIXME? this macro isn't necessary anymore

void SetMinMaxSize (edict_t *e, float *min, float *max, qboolean rotate);


#ifdef HEXEN2_SUPPORT

#define HX_FRAME_TIME 0.05

// builtin functions used in pr_cmds.c and progs_H2.c:
void PF_break (void);
void PF_sin (void);
void PF_sqrt (void);
void PF_precache_sound (void);
void PF_precache_model(void);
void PF_precache_file (void);
void PF_vectoangles (void);

// builtin helper functions used in pr_cmds.c and progs_H2.c:
char *PF_VarString (int	first);
void PR_CheckEmptyString (const char *s);

#endif		// #ifdef HEXEN2_SUPPORT

/******JDH******/

//============================================================================

void PR_Init (void);
void PR_LoadProgs (void);

void PR_ExecuteProgram (func_t fnum);
void PR_Profile_f (cmd_source_t src);

void PR_WriteGlobals (FILE *f);
void PR_ParseGlobals (const char *data);
void PR_LoadEdicts (const char *data);

void PR_PrintEdicts_f (cmd_source_t src);

char *PR_NewString (const char *string);
	// returns a copy of the string allocated from the server's string heap

void PR_LogMissingModel (const char *name);

ddef_t *PR_FindGlobal (const char *name);

#define PRFF_IGNORECASE 1
#define PRFF_NOBUILTINS 2
#define PRFF_NOPARAMS   4
#define PRFF_NOPARTIALS 8
#define PR_FindFunction(name, flags) PR_FindNextFunction(NULL, (name), (flags) | PRFF_NOPARTIALS)
dfunction_t *PR_FindNextFunction (int *start, const char *name, int flags);

edict_t *ED_Alloc (void);
void ED_Free (edict_t *ed);
void ED_ClearEdict (edict_t *e);

#define ED_Print(ed) ED_PrintWithOffset((ed),0)
void ED_PrintWithOffset (const edict_t *ed, int numoffset);		// JDH
void ED_PrintNum (int ent);

void ED_Write (FILE *f, const edict_t *ed);
const char *ED_ParseEdict (const char *data, edict_t *ent);

float ED_GetFieldValue (const edict_t *ed, const char *name);
void ED_SetFieldValue (edict_t *ed, const char *name, float val);

//define EDICT_NUM(n) ((edict_t *)(sv.edicts + (n)*pr_edict_size))
//define NUM_FOR_EDICT(e) (((byte *)(e) - sv.edicts)/pr_edict_size)

edict_t *EDICT_NUM (int n);
int NUM_FOR_EDICT (const edict_t *e);

#define	NEXT_EDICT(e)	((edict_t *)((byte *)e + pr_edict_size))

#define	EDICT_TO_PROG(e) ((byte *)e - (byte *)sv.edicts)
#define PROG_TO_EDICT(e) ((edict_t *)((byte *)sv.edicts + e))

//============================================================================

#define	G_FLOAT(o)	(pr_globals[o])
#define	G_INT(o)	(*(int *)&pr_globals[o])
#define	G_EDICT(o)	((edict_t *)((byte *)sv.edicts+ *(int *)&pr_globals[o]))
#define G_EDICTNUM(o)	NUM_FOR_EDICT(G_EDICT(o))
#define	G_VECTOR(o)	(&pr_globals[o])
#define	G_STRING(o)	(pr_strings + *(string_t *)&pr_globals[o])
#define	G_FUNCTION(o)	(*(func_t *)&pr_globals[o])

/* JDH - these won't work as is with field remapping
#define	E_FLOAT(e,o)	(((float*)&e->v)[o])
#define	E_INT(e,o)	(*(int *)&((float*)&e->v)[o])
#define	E_VECTOR(e,o)	(&((float*)&e->v)[o])
#define	E_STRING(e,o)	(pr_strings + *(string_t *)&((float*)&e->v)[o])
*/
extern	int		type_size[8];

typedef	void (*builtin_t) (void);
extern	builtin_t	*pr_builtins;
extern	int		pr_numbuiltins;

// 2001-09-14 Enhanced BuiltIn Function System (EBFS) by Maddes  start

typedef struct ebfs_builtin_s
{
	int			default_funcno;
	char		*funcname;
	builtin_t	function;
	int			funcno;
} ebfs_builtin_t;

extern ebfs_builtin_t	pr_ebfs_builtins[];
extern int				pr_ebfs_numbuiltins;

#define PR_DEFAULT_FUNCNO_BUILTIN_FIND	100

extern cvar_t	pr_builtin_find;
extern cvar_t	pr_builtin_remap;

#define PR_DEFAULT_FUNCNO_EXTENSION_FIND	99	// 2001-10-20 Extension System by Lord Havoc/Maddes

// 2001-09-14 Enhanced BuiltIn Function System (EBFS) by Maddes  end

extern ebfs_builtin_t	pr_builtins_beta[];
extern int				pr_numbuiltins_beta;

#ifdef HEXEN2_SUPPORT
extern ebfs_builtin_t	pr_builtins_H2[];
extern int				pr_numbuiltins_H2;
#endif

extern	int		pr_argc;

extern	qboolean	pr_trace;
extern	dfunction_t	*pr_xfunction;
extern	int			pr_xstatement;

extern	unsigned short	pr_crc;

void PR_RunError (const char *error, ...);

