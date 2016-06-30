/* Copyright (C) 2003-2015 LiveCode Ltd.

This file is part of LiveCode.

LiveCode is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License v3 as published by the Free
Software Foundation.

LiveCode is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with LiveCode.  If not see <http://www.gnu.org/licenses/>.  */

#include "prefix.h"

#include "globdefs.h"
#include "filedefs.h"
#include "objdefs.h"
#include "parsedef.h"
#include "mcio.h"

//#include "execpt.h"
#include "dispatch.h"
#include "stack.h"
#include "tooltip.h"
#include "card.h"
#include "field.h"
#include "button.h"
#include "image.h"
#include "aclip.h"
#include "vclip.h"
#include "stacklst.h"
#include "mcerror.h"
#include "hc.h"
#include "util.h"
#include "param.h"
#include "debug.h"
#include "statemnt.h"
#include "funcs.h"
#include "magnify.h"
#include "sellst.h"
#include "undolst.h"
#include "styledtext.h"
#include "property.h"
#include "osspec.h"

#include "system.h"
#include "globals.h"
#include "license.h"
#include "mode.h"
#include "revbuild.h"
#include "deploy.h"
#include "capsule.h"
#include "player.h"

#if defined(_WINDOWS_DESKTOP)
#include "w32prefix.h"
#include "w32compat.h"
#elif defined(_MAC_DESKTOP)
#include "osxprefix.h"
#endif

#include "resolution.h"
#include "libscript/script.h"

////////////////////////////////////////////////////////////////////////////////
//
//  Globals specific to STANDALONE mode
//

extern bool MCFiltersDecompress(MCDataRef p_source, MCDataRef& r_result);

// The MCDeployProjectInfo structure is generated by the internal 'deploy'
// command when a standalone is built. It contains all the information
// pertaining to the built application, and is used at runtime to locate, validate
// and launch the user's standalone application.
//
// The structure is placed in a specific executable 'section' which the deploy
// command can locate and modify.
//

struct MCCapsuleInfo
{
	uint32_t size;
	uint32_t data[3];
};

#if defined(_WINDOWS)
#pragma section(".project", read, discard)
__declspec(allocate(".project")) volatile MCCapsuleInfo MCcapsule = {0};
#elif defined(_LINUX)
__attribute__((section(".project"))) volatile MCCapsuleInfo MCcapsule = {0};
#elif defined(_MACOSX)
__attribute__((section("__PROJECT,__project"))) volatile MCCapsuleInfo MCcapsule = {0};
#elif defined(TARGET_SUBPLATFORM_IPHONE)
__attribute__((section("__PROJECT,__project"))) volatile MCCapsuleInfo MCcapsule = {0};
#elif defined(TARGET_SUBPLATFORM_ANDROID)
__attribute__((section(".project"))) volatile MCCapsuleInfo MCcapsule = {0};
#elif defined(TARGET_PLATFORM_MOBILE)
MCCapsuleInfo MCcapsule = {0};
#elif defined(__EMSCRIPTEN__)
MCCapsuleInfo MCcapsule = {0};
#endif

MCLicenseParameters MClicenseparameters =
{
	NULL, NULL, NULL, kMCLicenseClassNone, 0,
	0, 0, 0, 0,
	0,
	NULL,
};

// We don't include error string in this mode
const char *MCparsingerrors = "";
const char *MCexecutionerrors = "";

////////////////////////////////////////////////////////////////////////////////
//
//  Property tables specific to STANDALONE mode
//

MCObjectPropertyTable MCObject::kModePropertyTable =
{
	nil,
	0,
	nil,
};

MCObjectPropertyTable MCStack::kModePropertyTable =
{
	nil,
	0,
	nil,
};

MCPropertyTable MCProperty::kModePropertyTable =
{
	0,
	nil,
};

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of MCDispatch::startup method for STANDALONE mode.
//

extern IO_stat readheader(IO_handle& stream, char *version);
extern void send_startup_message(bool p_do_relaunch = true);

extern void add_simulator_redirect(const char *);
extern void add_ios_fontmap(const char *);

// This structure contains the information we collect from reading in the
// project.
struct MCStandaloneCapsuleInfo
{
	uint32_t origin;
	uint32_t execution_timeout;
	
	uint32_t banner_timeout;
	MCString banner_stackfile;
	
	MCStack *stack;
	bool done;
};

bool MCStandaloneCapsuleReadString(IO_handle p_stream, uint32_t p_length, MCStringRef &r_string)
{
	bool t_success;
	t_success = true;
	
	char *t_cstring;
	t_cstring = nil;
	
	if (t_success)
		t_success = MCMemoryAllocate(p_length, t_cstring);
	
	if (t_success)
		t_success = IO_read(t_cstring, p_length, p_stream) == IO_NORMAL;
	
	if (t_success)
		t_success = MCStringCreateWithCString(t_cstring, r_string);
	
	if (t_cstring != nil)
		MCMemoryDeallocate(t_cstring);
	
	return t_success;
}

bool MCStandaloneCapsuleCallback(void *p_self, const uint8_t *p_digest, MCCapsuleSectionType p_type, uint32_t p_length, IO_handle p_stream)
{
	MCStandaloneCapsuleInfo *self;
	self = static_cast<MCStandaloneCapsuleInfo *>(p_self);
	
	// If we've already seen the epilogue, we are done.
	if (self -> done)
	{
		MCresult -> sets("unexpected data encountered");
		return false;
	}

	switch(p_type)
	{
	case kMCCapsuleSectionTypeEpilogue:
		self -> done = true;
		break;

	case kMCCapsuleSectionTypePrologue:
	{
		MCCapsulePrologueSection t_prologue;
		if (IO_read(&t_prologue, sizeof(t_prologue), p_stream) != IO_NORMAL)
		{
			MCresult -> sets("failed to read standalone prologue");
			return false;
		}
		
		self -> execution_timeout = MCSwapInt32NetworkToHost(t_prologue . program_timeout);
		self -> banner_timeout = MCSwapInt32NetworkToHost(t_prologue . banner_timeout);
	}
	break;

	case kMCCapsuleSectionTypeRedirect:
	{
		char *t_redirect;
		t_redirect = new char[p_length];
		if (IO_read(t_redirect, p_length, p_stream) != IO_NORMAL)
		{
			MCresult -> sets("failed to read redirect ref");
			return false;
		}
		
#ifdef TARGET_SUBPLATFORM_IPHONE
		add_simulator_redirect(t_redirect);
#endif

		delete t_redirect;
	}
	break;
            
    case kMCCapsuleSectionTypeFontmap:
    {
        char *t_fontmap;
        t_fontmap = new char[p_length];
        if (IO_read(t_fontmap, p_length, p_stream) != IO_NORMAL)
        {
            MCresult -> sets("failed to read fontmap");
            return false;
        }
        
#ifdef TARGET_SUBPLATFORM_IPHONE
        // The font mapping is only viable (and needed) on iOS
        add_ios_fontmap(t_fontmap);
#endif
        
        delete[] t_fontmap;
    }
    break;
        
	case kMCCapsuleSectionTypeStack:
		if (MCdispatcher -> readstartupstack(p_stream, self -> stack) != IO_NORMAL)
		{
			MCresult -> sets("failed to read standalone stack");
			return false;
		}
		
		// MW-2012-10-25: [[ Bug ]] Make sure we set these to the main stack so that
		//   the startup script and such work.
		MCstaticdefaultstackptr = MCdefaultstackptr = self -> stack;
	break;
			
	case kMCCapsuleSectionTypeExternal:
	{
		MCAutoStringRef t_external_str;
		if (!MCStandaloneCapsuleReadString(p_stream, p_length, &t_external_str))
		{
			MCresult -> sets("failed to read external ref");
			return false;
		}
		
		if (!MCdispatcher -> loadexternal(*t_external_str))
		{
			MCresult -> sets("failed to load external");
			return false;
		}
	}
	break;

    // AL-2015-02-10: [[ Standalone Inclusions ]] Fetch a resource mapping and add it to the array stored in MCdispatcher.
    case kMCCapsuleSectionTypeLibrary:
    {
		MCAutoStringRef t_mapping_str;
		if (!MCStandaloneCapsuleReadString(p_stream, p_length, &t_mapping_str))
        {
            MCresult -> sets("failed to read library mapping");
            return false;
        }
        
        MCdispatcher -> addlibrarymapping(*t_mapping_str);
    }
        break;
            
	case kMCCapsuleSectionTypeStartupScript:
	{
		MCAutoStringRef t_script_str;
		if (!MCStandaloneCapsuleReadString(p_stream, p_length, &t_script_str))
		{
			MCresult -> sets("failed to read startup script");
			return false;
		}

		// Execute the startup script at this point since we have loaded
		// all stacks.
		self -> stack -> domess(*t_script_str);
	}
	break;
			
	case kMCCapsuleSectionTypeAuxiliaryStack:
	{
		MCStack *t_aux_stack;
		if (MCdispatcher -> readfile(NULL, NULL, p_stream, t_aux_stack) != IO_NORMAL)
		{
            MCresult -> sets("failed to read auxiliary stack");
			return false;
		}
	}
	break;
            
    case kMCCapsuleSectionTypeModule:
    {
		MCAutoByteArray t_module_data;
		if (!t_module_data.New(p_length))
		{
			MCresult -> sets("out of memory");
			return false;
		}
		
        if (IO_read(t_module_data.Bytes(), p_length, p_stream) != IO_NORMAL)
        {
            MCresult -> sets("failed to read module");
            return false;
        }
    
        bool t_success;
        t_success = true;
        
        MCStreamRef t_stream;
        t_stream = nil;
        if (t_success)
            t_success = MCMemoryInputStreamCreate(t_module_data.Bytes(), p_length, t_stream);
        
        MCScriptModuleRef t_module;
        if (t_success)
            t_success = MCScriptCreateModuleFromStream(t_stream, t_module);
        
        if (t_stream != nil)
            MCValueRelease(t_stream);
        
        if (!t_success)
        {
            MCresult -> sets("failed to load module");
            return false;
        }
        
        extern bool MCEngineAddExtensionFromModule(MCScriptModuleRef module);
        if (!MCEngineAddExtensionFromModule(t_module))
        {
            MCScriptReleaseModule(t_module);
            return false;
        }
    }
    break;

	case kMCCapsuleSectionTypeDigest:
		uint8_t t_read_digest[16];
		if (IO_read(t_read_digest, 16, p_stream) != IO_NORMAL)
		{
			MCresult -> sets("failed to read standalone checksum");
			return false;
		}
		if (memcmp(t_read_digest, p_digest, 16) != 0)
		{
			MCresult -> sets("standalone checksum mismatch");
			return false;
		}
		break;
    
    case kMCCapsuleSectionTypeLicense:
    {
        char t_edition_byte;
        if (IO_read(&t_edition_byte, 1, p_stream) != IO_NORMAL)
        {
            MCresult -> sets("failed to read license");
            return false;
        }

        bool t_success;
        t_success = true;
        
        // The edition encoding is engine version / IDE engine version
        // specific - for now its just a byte with value 0-3.
        if (t_edition_byte == 1)
            MClicenseparameters . license_class = kMCLicenseClassCommunity;
        else if (t_edition_byte == 2)
            MClicenseparameters . license_class = kMCLicenseClassCommercial;
        else if (t_edition_byte == 3)
            MClicenseparameters . license_class = kMCLicenseClassProfessional;
        else
            MClicenseparameters . license_class = kMCLicenseClassNone;
    }
    break;
			
	case kMCCapsuleSectionTypeBanner:
	{
		void *t_data;
		if (MCMemoryAllocate(p_length, t_data))
		{
			if (IO_read(t_data, p_length, p_stream) == IO_NORMAL)
				self -> banner_stackfile . set((char *)t_data, p_length);
			else
			{
				MCMemoryDeallocate(t_data);
				MCresult -> sets("failed to read banner stack");
				return false;
			}
		}
		else
		{
			MCresult -> sets("out of memory");
			return false;
		}
	}
	break;
	
	default:
		MCresult -> sets("unrecognized section encountered");
		return false;
	}
	
	return true;
}

#ifdef _MOBILE

#ifdef TARGET_SUBPLATFORM_ANDROID
extern void MCAndroidEngineRemoteCall(const char *, const char *, void *, ...);
#endif

IO_stat MCDispatch::startup(void)
{
	startdir = MCS_getcurdir();
    /* UNCHECKED */ MCStringConvertToCString(MCcmd, enginedir);
	char *eptr = strrchr(enginedir, PATH_SEPARATOR);
	if (eptr != NULL)
		*eptr = '\0';
	else
		*enginedir = '\0';

	// set up image cache before the first stack is opened
	MCCachedImageRep::init();
	
#if defined(_DEBUG) || defined(_SHARK)
	// MW-2013-06-13: [[ CloneAndRun ]] If there is no capsule and we are in profile/debug mode then
	//   use a file directly.
	if (MCcapsule . size == 0)
	{
		IO_handle t_stream;
	
		// In debug mode, we (for now) load a fixed stack
#if defined(TARGET_SUBPLATFORM_ANDROID)
        extern IO_handle android_get_mainstack_stream();
        t_stream = android_get_mainstack_stream();
#else
        MCAutoStringRef t_path;
        uindex_t t_last_slash;
        /* UNCHECKED */ MCStringLastIndexOfChar(MCcmd, '/', UINDEX_MAX, kMCCompareExact, t_last_slash);
        // temporary fix until ranged formats for stringrefs is working
        MCAutoStringRef t_dir;
        /* UNCHECKED */ MCStringCopySubstring(MCcmd, MCRangeMake(0, t_last_slash), &t_dir);
        /* UNCHECKED */ MCStringFormat(&t_path, "%@/iphone_test.rev", *t_dir);

        t_stream = MCS_open(*t_path, kMCOpenFileModeRead, False, False, 0);
#endif
		
		if (t_stream == NULL)
        {
		    MCresult->sets("unable to open startup stack");
			return IO_ERROR;
        }
		
		MCStack *t_stack;
		if (readstartupstack(t_stream, t_stack) != IO_NORMAL)
        {
		    MCresult->sets("unable to read startup stack");
			return IO_ERROR;
        }
		
		MCS_close(t_stream);
		
		MCdefaultstackptr = MCstaticdefaultstackptr = t_stack;
		
		t_stack -> extraopen(false);
		
		// Resolve parent scripts *after* we've loaded aux stacks.
		if (t_stack -> getextendedstate(ECS_USES_PARENTSCRIPTS))
			t_stack -> resolveparentscripts();
		
		MCscreen->resetcursors();
		MCImage::init();
		
#ifdef TARGET_SUBPLATFORM_ANDROID
        MCdispatcher -> loadexternal(MCSTR("revzip"));
        MCdispatcher -> loadexternal(MCSTR("revdb"));
        MCdispatcher -> loadexternal(MCSTR("revxml"));
        MCdispatcher -> loadexternal(MCSTR("dbsqlite"));
        MCdispatcher -> loadexternal(MCSTR("dbmysql"));
#else
        MCdispatcher -> loadexternal(MCSTR("revzip.dylib"));
        MCdispatcher -> loadexternal(MCSTR("revdb.dylib"));
#endif
		
		// MW-2010-12-18: Startup message / stack init now down in 'main'
		return IO_NORMAL;
	}
#endif
	
	// The info structure that will be filled in while parsing the capsule.
	MCStandaloneCapsuleInfo t_info;
	memset(&t_info, 0, sizeof(MCStandaloneCapsuleInfo));

	// Create a capsule and fill with the standalone data
	MCCapsuleRef t_capsule;
	t_capsule = nil;
	if (!MCCapsuleOpen(MCStandaloneCapsuleCallback, &t_info, t_capsule))
		return IO_ERROR;
	
	if (((MCcapsule . size) & (1 << 31U)) == 0)
	{
		// Capsule is not spilled - just use the project section.
		if (!MCCapsuleFillNoCopy(t_capsule, (const void *)&MCcapsule . data[0], MCcapsule . size, true))
		{
			MCCapsuleClose(t_capsule);
			return IO_ERROR;
		}
	}
	else
	{
		// Capsule is spilled fill from:
		//   0..2044 from project section
		//   spill file
		//   rest from project section
		MCAutoStringRef t_spill;
        /* UNCHECKED */ MCStringFormat(&t_spill, "%@.dat", MCcmd);
		if (!MCCapsuleFillNoCopy(t_capsule, (const void *)&MCcapsule . data, 2044, false) ||
			!MCCapsuleFillFromFile(t_capsule, *t_spill, 0, false) ||
			!MCCapsuleFillNoCopy(t_capsule, (const uint8_t *)&MCcapsule . data + 2044, 2048, true))
		{
			MCCapsuleClose(t_capsule);
			return IO_ERROR;
		}
	}
	
	// Process the capsule
	if (!MCCapsuleProcess(t_capsule))
	{
		MCCapsuleClose(t_capsule);
		return IO_ERROR;
	}

	MCdefaultstackptr = MCstaticdefaultstackptr = t_info . stack;
	MCCapsuleClose(t_capsule);
	
	// If there is a banner to display, let it do so until the timeout.
	if (t_info . banner_timeout > 0)
	{
#if defined(TARGET_SUBPLATFORM_ANDROID)
		MCAndroidEngineRemoteCall("showSplashScreen", "v", nil);
#endif
		
		// Run the banner timeout loop.
		real8 t_finish_time;
		t_finish_time = MCS_time() + t_info . banner_timeout;
		while(!MCquit)
		{
			real8 t_duration;
			t_duration = t_finish_time - MCS_time();
			if (t_duration <= 0.0)
				break;
			
			MCscreen -> wait(t_duration, False, True);
		}
	}
	
	// If there is an execution timeout, schedule an internal message to
	// terminate the app after that time.
	if (t_info . execution_timeout > 0)
		MCscreen -> addtimer(this, MCM_internal, t_info . execution_timeout * 1000);
	
	if (!MCquit)
	{
		t_info . stack -> extraopen(false);
	
		// Resolve parent scripts *after* we've loaded aux stacks.
		if (t_info . stack -> getextendedstate(ECS_USES_PARENTSCRIPTS))
			t_info . stack -> resolveparentscripts();
		
		MCscreen->resetcursors();
		MCImage::init();
	}
	
	// MW-2010-12-18: Startup message / stack init now down in 'main'
    
	return IO_NORMAL;
}

#elif defined(__EMSCRIPTEN__)

#include "stacksecurity.h"

#define kMCEmscriptenBootStackFilename "/boot/standalone/__boot.livecode"
#define kMCEmscriptenStartupStackFilename "/boot/__startup.livecode"

IO_stat
MCDispatch::startup()
{
	/* The standalone data should already have been unpacked by now */

	/* Load & run the startup script in a temporary stack */
	MCStack *t_startup_stack = nil;
	if (IO_NORMAL != MCdispatcher->loadfile(MCSTR(kMCEmscriptenStartupStackFilename),
	                                        t_startup_stack))
	{
		MCresult->sets("failed to load startup stack");
		return IO_ERROR;
	}

	/* Check the stack */
	if (!MCStackSecurityEmscriptenStartupCheck(t_startup_stack))
	{
		MCresult->sets("startup stack checks failed");
		return IO_ERROR;
	}

	MCdefaultstackptr = MCstaticdefaultstackptr = t_startup_stack;

	/* Attempt to run the startup handler */
	if (ES_NORMAL != t_startup_stack->message(MCM_start_up, nil, false, true))
	{
		/* Handler couldn't be run at all, or threw an error */
		MCresult->sets("failed to run startup stack");
	}
	/* The startup stack *should* set the result on failure */
	{
		MCExecContext ctxt;
		MCAutoValueRef t_result;
		MCresult->eval(ctxt, &t_result);
		if (!MCValueIsEmpty(*t_result))
		{
			return IO_ERROR;
		}
	}

	/* Delete the startup stack */
	MCdispatcher->destroystack(t_startup_stack, true);

	/* Load the initial stack */
	MCStack *t_stack;
	if (IO_NORMAL != MCdispatcher->loadfile(MCSTR(kMCEmscriptenBootStackFilename),
	                                        t_stack))
	{
		MCresult->sets("failed to read initial stackfile");
		return IO_ERROR;
	}

	MCdefaultstackptr = MCstaticdefaultstackptr = t_stack;

	/* Complete startup tasks and send the startup message */

	MCModeResetCursors();
	MCImage::init();

	MCallowinterrupts = false;

	MCdefaultstackptr->extraopen(false);

	send_startup_message();

	if (!MCquit)
	{
		MCdefaultstackptr->open();
	}

	return IO_NORMAL;
}

#else

IO_stat MCDispatch::startup(void)
{
    char *t_mccmd;
    /* UNCHECKED */ MCStringConvertToCString(MCcmd, t_mccmd);
	startdir = MCS_getcurdir();
	enginedir = t_mccmd;
	char *eptr = strrchr(enginedir, PATH_SEPARATOR);
	if (eptr != NULL)
		*eptr = '\0';
	else
		*enginedir = '\0';
	char *openpath = t_mccmd; //point to MCcmd string

#ifdef _DEBUG
	// MW-2013-06-13: [[ CloneAndRun ]] When compiling in DEBUG mode, first check
	//   to see if there is an environment TEST_STACK specified; otherwise read
	//   from the capsule.
	
#ifdef _WINDOWS
	// This little snippet of code allows an easy way to attach VS to a standalone
	// instance to debug startup.
	/*while(!IsDebuggerPresent())
		Sleep(50);
	Sleep(250);
	DebugBreak();*/
#endif

    MCAutoStringRef t_env;
    if (MCS_getenv(MCSTR("TEST_STACK"), &t_env) && !MCStringIsEmpty(*t_env))
    {
	    MCStack *t_stack;
	    if (MCdispatcher->loadfile(*t_env, t_stack) != IO_NORMAL)
	    {
		    MCresult->sets("failed to read TEST_STACK stackfile");
		    return IO_ERROR;
	    }

		MCdefaultstackptr = MCstaticdefaultstackptr = t_stack;

		MCModeResetCursors();
		MCImage::init();

		MCallowinterrupts = False;

		t_stack -> extraopen(false);

		send_startup_message();
		if (!MCquit)
			t_stack -> open();

		return IO_NORMAL;
	}
#endif
	
	// MW-2013-11-07: [[ CmdLineStack ]] If there is a capsule, load the mainstack
	//   from that. Otherwise, if there is at least one argument, load that as the
	//   stack. Otherwise it's an error.
	MCStack *t_mainstack;
	t_mainstack = nil;
	
	// The info structure that will be filled in while parsing the capsule.
	MCStandaloneCapsuleInfo t_info;
	memset(&t_info, 0, sizeof(MCStandaloneCapsuleInfo));
	
	if (MCcapsule . size != 0)
	{

		// Create a capsule and fill with the standalone data
		MCCapsuleRef t_capsule;
		t_capsule = nil;
		if (!MCCapsuleOpen(MCStandaloneCapsuleCallback, &t_info, t_capsule))
			return IO_ERROR;

		if (((MCcapsule . size) & (1U << 31)) == 0)
		{
			// Capsule is not spilled - just use the project section.
			// MW-2010-05-08: Capsule size includes 'size' field, so need to adjust
			if (!MCCapsuleFillNoCopy(t_capsule, (const void *)&MCcapsule . data, MCcapsule . size - sizeof(uint32_t), true))
			{
				MCCapsuleClose(t_capsule);
				return IO_ERROR;
			}
		}
		else
		{
			// Capsule is spilled fill from:
			//   0..2044 from project section
			//   spill file
			//   rest from project section
			MCAutoStringRef t_spill;
			/* UNCHECKED */ MCStringFormat(&t_spill, "%@.dat", MCcmd);
			if (!MCCapsuleFillFromFile(t_capsule, *t_spill, 0, true))
			{
				MCCapsuleClose(t_capsule);
				return IO_ERROR;
			}
		}

		// Process the capsule
		if (!MCCapsuleProcess(t_capsule))
		{
			MCCapsuleClose(t_capsule);
			return IO_ERROR;
		}
		
		MCCapsuleClose(t_capsule);
		
		t_mainstack = t_info . stack;
	}
	else if (MCnstacks > 1 && MClicenseparameters . license_class == kMCLicenseClassCommunity)
	{
		MCStack *sptr;
		if (MCdispatcher -> loadfile(MCstacknames[1], sptr) != IO_NORMAL)
		{
			MCresult -> sets("failed to read stackfile");
			return IO_ERROR;
		}
		
		t_mainstack = sptr;
		
		MCMemoryMove(MCstacknames, MCstacknames + 1, sizeof(MCStack *) * (MCnstacks - 1));
		MCnstacks -= 1;
	}
	else
	{
		MCresult -> sets("no stackfile to run");
		return IO_ERROR;
	}
	
	MCdefaultstackptr = MCstaticdefaultstackptr = t_mainstack;

	// Initialization required.
	MCModeResetCursors();
	MCImage::init();

	// MW-2010-10-22: [[ Bug 8352 ]] Make sure allowInterrupts is off by default.
	MCallowinterrupts = False;

	// Display the banner (if any).
	if (t_info . banner_timeout > 0)
	{
		MCStack *t_banner_stack;
		t_banner_stack = nil;
		
		MCDataRef t_decompressed;
		MCDataRef t_compressed;
		t_compressed = nil;
		t_decompressed = nil;
		if (!MCDataCreateWithBytes((const byte_t*)t_info.banner_stackfile.getstring(),
								   t_info.banner_stackfile.getlength(),
								   t_compressed) ||
			!MCFiltersDecompress(t_compressed,
								 t_decompressed))
		{
			MCValueRelease(t_compressed);
			MCresult -> sets("error decoding banner stack");
			return IO_ERROR;
		}
		MCValueRelease(t_compressed);
		
		IO_handle t_stream;
		t_stream = MCS_fakeopen(MCDataGetBytePtr(t_decompressed),
								MCDataGetLength(t_decompressed));
		if (MCdispatcher -> readfile(nil, nil, t_stream, t_banner_stack) != IO_NORMAL)
		{
			MCresult -> sets("invalid banner stack");
			return IO_ERROR;
		}
		MCS_close(t_stream);
		MCValueRelease(t_decompressed);
		
		MCallowinterrupts = False;
		
		t_banner_stack -> setfilename(MCcmd);
		MCdefaultstackptr = MCstaticdefaultstackptr = stacks;
		
		MCExecContext ctxt;
		MCExecValue t_value;
		t_value . int_value = (integer_t)t_info . banner_timeout;
		t_value . type = kMCExecValueTypeInt;
		t_banner_stack -> setcustomprop(ctxt, kMCEmptyName, MCNAME("uTimeout"), nil, t_value);
		
		t_banner_stack -> open();
		double t_end_time;
		t_end_time = MCS_time() + t_info . banner_timeout;
		while(MCS_time() < t_end_time)
			MCscreen -> wait(t_end_time - MCS_time(), True, False);
		
		destroystack(t_banner_stack, True);
		MCtopstackptr = NULL;
		
		MCMemoryDeallocate((void *)t_info . banner_stackfile . getstring());
	}
	
	// If there is an execution timeout, schedule an internal message to
	// terminate the app after that time.
	if (t_info . execution_timeout > 0)
		MCscreen -> addtimer(this, MCM_internal, t_info . execution_timeout * 1000);
	
	// Now open the main stack.
	t_mainstack-> extraopen(false);
	send_startup_message();
	if (!MCquit)
		t_mainstack -> open();

	return IO_NORMAL;
}
#endif

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of MCStack::mode* hooks for STANDALONE mode.
//

#ifdef LEGACY_EXEC
Exec_stat MCStack::mode_getprop(uint4 parid, Properties which, MCExecPoint &ep, MCStringRef carray, Boolean effective)
{
	return ES_NOT_HANDLED;
}

Exec_stat MCStack::mode_setprop(uint4 parid, Properties which, MCExecPoint &ep, MCStringRef cprop, MCStringRef carray, Boolean effective)
{
	return ES_NOT_HANDLED;
}
#endif

void MCStack::mode_load(void)
{
}

void MCStack::mode_getrealrect(MCRectangle& r_rect)
{
	MCscreen->getwindowgeometry(window, r_rect);
}

void MCStack::mode_takewindow(MCStack *other)
{
}

void MCStack::mode_takefocus(void)
{
	MCscreen->setinputfocus(window);
}

bool MCStack::mode_needstoopen(void)
{
	return true;
}

void MCStack::mode_setgeom(void)
{
}

void MCStack::mode_setcursor(void)
{
	MCscreen->setcursor(window, cursor);
}

bool MCStack::mode_openasdialog(void)
{
	return true;
}

void MCStack::mode_closeasdialog(void)
{
}

void MCStack::mode_openasmenu(MCStack *grab)
{
}

void MCStack::mode_closeasmenu(void)
{
}

void MCStack::mode_constrain(MCRectangle& rect)
{
}

#ifdef _WINDOWS
MCSysWindowHandle MCStack::getrealwindow(void)
{
	return window->handle.window;
}

MCSysWindowHandle MCStack::getqtwindow(void)
{
	return window->handle.window;
}
#endif

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of MCObject::mode_get/setprop for STANDALONE mode.
//

#ifdef LEGACY_EXEC
Exec_stat MCObject::mode_getprop(uint4 parid, Properties which, MCExecPoint &ep, MCStringRef carray, Boolean effective)
{
	return ES_NOT_HANDLED;
}
#endif
////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of MCProperty::mode_eval/mode_set for STANDALONE mode.
//
#ifdef LEGACY_EXEC
Exec_stat MCProperty::mode_set(MCExecPoint& ep)
{
	return ES_NOT_HANDLED;
}

Exec_stat MCProperty::mode_eval(MCExecPoint& ep)
{
	return ES_NOT_HANDLED;
}
#endif
////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of mode hooks for STANDALONE mode.
//

// In standalone mode, the standalone stack built into the engine cannot
// be saved.
IO_stat MCModeCheckSaveStack(MCStack *sptr, const MCStringRef p_filename)
{
	if (sptr == MCdispatcher -> getstacks())
	{
		MCresult->sets("can't save into standalone");
		return IO_ERROR;
	}

	return IO_NORMAL;
}

// In standalone mode, the environment depends on various command-line/runtime
// globals.
MCNameRef MCModeGetEnvironment(void)
{
#ifdef _MOBILE
	return MCN_mobile;
#else
	// MW-2011-09-19: [[ Bug 9734 ]] If in '-ui' mode, return 'command line'.
	if (MCnoui)
		return MCN_command_line;

	if (MCnofiles)
		return MCN_helper_application;

	return MCN_standalone_application;
#endif
}

uint32_t MCModeGetEnvironmentType(void)
{
	if (MCnofiles)
		return kMCModeEnvironmentTypeHelper;
	return kMCModeEnvironmentTypeDesktop;
}

// SN-2015-01-16: [[ Bug 14295 ]] Get the standalone, redirected resources folder.
void MCModeGetResourcesFolder(MCStringRef &r_resources_folder)
{
    MCS_getresourcesfolder(true, r_resources_folder);
}

// In standalone mode, we are never licensed.
bool MCModeGetLicensed(void)
{
	return false;
}

// In standalone mode, the executable is $0 if there is an embedded stack.
bool MCModeIsExecutableFirstArgument(void)
{
	return true;
}

// Desktop standalone have command line arguments - not mobile platforms
bool MCModeHasCommandLineArguments(void)
{
#ifdef _MOBILE
    return false;
#else
    return true;
#endif
}

// Standalones have environment variables
bool
MCModeHasEnvironmentVariables()
{
	return true;
}

// In standalone mode, we only automatically open stacks if there isn't an
// embedded stack.
bool MCModeShouldLoadStacksOnStartup(void)
{
	return false;
}

// In standalone mode, we try to work out what went wrong...
void MCModeGetStartupErrorMessage(MCStringRef& r_caption, MCStringRef& r_text)
{
	r_caption = MCSTR("Initialization Error");
	if (MCValueGetTypeCode(MCresult -> getvalueref()) == kMCValueTypeCodeString)
		r_text = MCValueRetain((MCStringRef)MCresult -> getvalueref());
	else
		r_text = MCSTR("unknown reason");
}

// In standalone mode, we can only set an object's script if has non-zero id.
bool MCModeCanSetObjectScript(uint4 obj_id)
{
	return obj_id != 0;
}

// In standalone mode, we must check the old CANT_STANDALONE flag.
bool MCModeShouldCheckCantStandalone(void)
{
	return true;
}

// The standalone mode doesn't have a message box redirect feature
bool MCModeHandleMessageBoxChanged(MCExecContext& ctxt, MCStringRef p_msg)
{
	return false;
}

// The standalone mode causes a relaunch message.
bool MCModeHandleRelaunch(MCStringRef &r_id)
{
#ifdef _WINDOWS
	bool t_do_relaunch;
	t_do_relaunch = MCdefaultstackptr -> hashandler(HT_MESSAGE, MCM_relaunch) == True;
	/* UNCHECKED */ MCStringCopy(MCNameGetString(MCdefaultstackptr -> getname()), r_id);
	return t_do_relaunch;
#else
	return false;
#endif
}

// The standalone mode's startup stack depends on whether it has a stack embedded.
const char *MCModeGetStartupStack(void)
{
	return NULL;
}

bool MCModeCanLoadHome(void)
{
	return false;
}

MCStatement *MCModeNewCommand(int2 which)
{
	return NULL;
}

MCExpression *MCModeNewFunction(int2 which)
{
	return NULL;
}

MCObject *MCModeGetU3MessageTarget(void)
{
	return MCdefaultstackptr -> getcard();
}

bool MCModeShouldQueueOpeningStacks(void)
{
	return MCscreen == NULL;
}

bool MCModeShouldPreprocessOpeningStacks(void)
{
	return false;
}

Window MCModeGetParentWindow(void)
{
	Window t_window;
	t_window = MCdefaultstackptr -> getwindow();
	if (t_window == NULL && MCtopstackptr != NULL)
		t_window = MCtopstackptr -> getwindow();
	return t_window;
}

bool MCModeCanAccessDomain(MCStringRef p_name)
{
	return false;
}

void MCModeQueueEvents(void)
{
}

Exec_stat MCModeExecuteScriptInBrowser(MCStringRef p_script)
{
	MCeerror -> add(EE_ENVDO_NOTSUPPORTED, 0, 0);
	return ES_ERROR;
}

bool MCModeMakeLocalWindows(void)
{
	return true;
}

void MCModeActivateIme(MCStack *p_stack, bool p_activate)
{
	MCscreen -> activateIME(p_activate);
}

void MCModeConfigureIme(MCStack *p_stack, bool p_enabled, int32_t x, int32_t y)
{
	if (!p_enabled)
		MCscreen -> clearIME(p_stack -> getwindow());
    else
        MCscreen -> configureIME(x, y);
}

void MCModeShowToolTip(int32_t x, int32_t y, uint32_t text_size, uint32_t bg_color, MCStringRef text_font, MCStringRef message)
{
}

void MCModeHideToolTip(void)
{
}

void MCModeResetCursors(void)
{
	MCscreen -> resetcursors();
}

bool MCModeCollectEntropy(void)
{
	return true;
}

// We return false here as at present, in standalones, the first stack does not
// sit in the message path of all stacks.
bool MCModeHasHomeStack(void)
{
	return false;
}

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of remote dialog methods
//

void MCRemoteFileDialog(MCStringRef p_title, MCStringRef p_prompt, MCStringRef *p_types, uint32_t p_type_count, MCStringRef p_initial_folder, MCStringRef p_initial_file, bool p_save, bool p_files, MCStringRef &r_value)
{
}

void MCRemoteColorDialog(MCStringRef p_title, uint32_t p_r, uint32_t p_g, uint32_t p_b, bool &r_chosen, MCColor &r_chosen_color)
{
}

void MCRemoteFolderDialog(MCStringRef p_title, MCStringRef p_prompt, MCStringRef p_initial, MCStringRef &r_value)
{
}

void MCRemotePrintSetupDialog(MCDataRef p_config_data, MCDataRef &r_reply_data, uint32_t &r_result)
{
}

void MCRemotePageSetupDialog(MCDataRef p_config_data, MCDataRef &r_reply_data, uint32_t &r_result)
{
}

#ifdef _MACOSX
uint32_t MCModePopUpMenu(MCMacSysMenuHandle p_menu, int32_t p_x, int32_t p_y, uint32_t p_index, MCStack *p_stack)
{
	return 0;
}
#endif

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of Windows-specific mode hooks for STANDALONE mode.
//

#ifdef TARGET_PLATFORM_WINDOWS

// MW-2014-04-22: [[ Bug 12237 ]] Attempt to attach to a console if available.
//   This shouldn't have any adverse consequences on anything as if the engine
//   isn't launched from the console (i.e. run from the desktop) it will be as
//   before; and if it is launched from the console then it will probably do
//   what is expected.
typedef BOOL (WINAPI *AttachConsolePtr)(DWORD id);
void MCModePreMain(void)
{
	HMODULE t_kernel;
	t_kernel = LoadLibraryA("kernel32.dll");
	if (t_kernel != nil)
	{
		void *t_attach_console;
		t_attach_console = GetProcAddress(t_kernel, "AttachConsole");
		if (t_attach_console != nil)
		{
			((AttachConsolePtr)t_attach_console)(-1);
			return;
		}
	}
}


void MCModeSetupCrashReporting(void)
{
}

bool MCModeHandleMessage(LPARAM lparam)
{
	return false;
}

bool MCPlayer::mode_avi_closewindowonplaystop()
{
	return true;
}

// IM-2014-08-08: [[ Bug 12372 ]] Only use pixel scaling in the standalone
// if dpiAwareness has been configured in the application manifest.
bool MCModeGetPixelScalingEnabled()
{
	bool t_success;
	t_success = true;

	unichar_t *t_value;
	t_value = nil;

	t_success = MCWin32QueryActCtxSettings(L"dpiAware", t_value);

	if (t_success)
		t_success = 0 == wcscmp(t_value, L"true");

	if (t_value != nil)
		MCMemoryDeallocate(t_value);

	return t_success;
}

#endif

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of Mac OS X-specific mode hooks for DEVELOPMENT mode.
//

#ifdef _MACOSX

bool MCModePreWaitNextEvent(Boolean anyevent)
{
	return false;
}

#endif

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of Linux-specific mode hooks for DEVELOPMENT mode.
//

#ifdef _LINUX

void MCModePreSelectHook(int& maxfd, fd_set& rfds, fd_set& wfds, fd_set& efds)
{
}

void MCModePostSelectHook(fd_set& rfds, fd_set& wfds, fd_set& efds)
{
}

#endif
