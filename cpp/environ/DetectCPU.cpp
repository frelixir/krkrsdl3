//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000-2007 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// CPU idetification / features detection routine
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "cpu_types.h"
#include "TVPDebug.h"
#include "TVPSystem.h"

#include "TVPThread.h"
#include "Exception.h"

/*
	Note: CPU clock measuring routine is in EmergencyExit.cpp, reusing
	hot-key watching thread.
*/

//---------------------------------------------------------------------------
extern "C"
{
	tjs_uint32 TVPCPUType = 0; // CPU type
    tjs_uint32 TVPCPUFeatures	=	0;
}

static bool TVPCPUChecked = false;
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// TVPGetCPUTypeForOne
//---------------------------------------------------------------------------
static void TVPGetCPUTypeForOne()
{
	try
	{
        TVPCPUFeatures = 0;

		//TVPCheckCPU(); // in detect_cpu.nas
	}
	catch(.../*EXCEPTION_EXECUTE_HANDLER*/)
	{
		// exception had been ocured
		throw Exception("CPU check failure.");
	}
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
static ttstr TVPDumpCPUInfo(tjs_int cpu_num)
{
	// dump detected cpu type
	ttstr features(TJS_N("(info) CPU #") + ttstr(cpu_num) +
		TJS_N(" : "));

	tjs_uint32 vendor = TVPCPUFeatures & TVP_CPU_VENDOR_MASK;

	TVPAddImportantLog(features);

	return features;
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
void TVPDetectCPU()
{
    if(TVPCPUChecked) return;
    TVPCPUChecked = true;
	TVPCPUFeatures= 0;
	TVPCPUType = 0;
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// jpeg and png loader support functions
//---------------------------------------------------------------------------
unsigned long MMXReady = 0;
extern "C"
{
	void CheckMMX(void)
	{
		TVPDetectCPU();
		MMXReady = TVPCPUType & TVP_CPU_HAS_MMX;
	}
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// TVPGetCPUType
//---------------------------------------------------------------------------
tjs_uint32 TVPGetCPUType()
{
	TVPDetectCPU();
	return TVPCPUType;
}
//---------------------------------------------------------------------------


