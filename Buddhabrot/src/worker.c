/*
    Copyright � 2017, The AROS Development Team. All rights reserved.
    $Id$
*/

#include <exec/tasks.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <clib/exec_protos.h>

#include "work.h"
#include "support.h"

#define DWORK(x)

/*
 * define a complex number with
 * real and imaginary parts
 */
typedef struct {
	double r;
	double i;
} complexno_t;

struct WorkersWork
{
    ULONG         workMax;
    ULONG         workOversamp;
    ULONG         workOver2;
    struct SignalSemaphore  *lock;
    complexno_t workTrajectories[];  
};

ULONG calculateTrajectory(struct WorkersWork *workload, double r, double i)
{
    double realNo, imaginaryNo, realNo2, imaginaryNo2, tmp;
    ULONG trajectory;

    /* Calculate trajectory */
    realNo = 0;
    imaginaryNo = 0;

    for(trajectory = 0; trajectory < workload->workMax; trajectory++)
    {
        /* Check if it's out of circle with radius 2 */
        realNo2 = realNo * realNo;
        imaginaryNo2 = imaginaryNo * imaginaryNo;

        if (realNo2 + imaginaryNo2 > 4.0)
            return trajectory;

        /* Next */
        tmp = realNo2 - imaginaryNo2 + r;
        imaginaryNo = 2.0 * realNo * imaginaryNo + i;
        realNo = tmp;

        /* Store */
        workload->workTrajectories[trajectory].r = realNo;
        workload->workTrajectories[trajectory].i = imaginaryNo;
    }

    return 0;
}

double x_0 = 0.0, y_0 = 0, size = 4.0;

void processWork(struct WorkersWork *workload, ULONG *workBuffer, ULONG workWidth, ULONG workHeight, ULONG workStart, ULONG workEnd, BOOL buddha)
{
    /*
    double xlo = -1.0349498063694267;
    double ylo = -0.36302123503184713;
    double xhi = -0.887179105732484;
    double yhi = -0.21779830509554143;
    */
    ULONG trajectoryLength;
    ULONG current;
    
    double x, y;
    double diff = size / ((double)(workWidth * workload->workOversamp));
    double y_base = size / 2.0 - (diff * 2.0 / size);
    double diff_sr = size / (double)workWidth;

    DWORK(
        bug("[SMP-Test:Worker] %s: Buffer @ 0x%p\n", __func__, workBuffer);
        bug("[SMP-Test:Worker] %s:           %dx%d\n", __func__, workWidth, workHeight);
        bug("[SMP-Test:Worker] %s: start : %d, end %d\n", __func__, workStart, workEnd);
    )

    for (current = workStart * workload->workOver2; current < (workEnd + 1) * workload->workOver2 - 1; current++)
    {
        ULONG val;

        /* Locate the point on the complex plane */
        x = x_0 + ((double)(current % (workWidth * workload->workOversamp))) * diff - size / 2.0;
        y = y_0 + ((double)(current / (workWidth * workload->workOversamp))) * diff - y_base;

        /* Calculate the points trajectory ... */
        trajectoryLength = calculateTrajectory(workload, x, y);

        if (buddha)
        {
            /* Update the display if it escapes */
            if (trajectoryLength > 0)
            {
                ULONG pos;
                int i;
                ObtainSemaphore(workload->lock);
                for(i = 0; i < trajectoryLength; i++)
                {
                    ULONG py = (workload->workTrajectories[i].r - y_0 + size / 2.0) / diff_sr;
                    ULONG px = (workload->workTrajectories[i].i - x_0 + y_base) / diff_sr;

                    pos = (ULONG)(workWidth * py + px);

                    if (pos > 0 && pos < (workWidth * workHeight))
                    {

                        val = workBuffer[pos];
                        
                        if (val < 0xfff)
                            val++;

                        workBuffer[pos] = val;

                    }
                }
                ReleaseSemaphore(workload->lock);
            }
        }
        else
        {
            (void)diff_sr;

            val = workBuffer[current / workload->workOver2];

            val+= 10*trajectoryLength; //(255 * trajectoryLength) / workload->workMax;
            
            if (val > 0xfff)
                val = 0xfff;

            workBuffer[current / workload->workOver2] = val;
        }
    }
}

/*
 * This Task processes work received
 */
void SMPTestWorker()
{
    struct Task *thisTask = FindTask(NULL);
    struct SMPWorker *worker = thisTask->tc_UserData;
    struct SMPWorkMessage *workMsg;
    struct WorkersWork    *workPrivate;
    BOOL doWork = TRUE, buddha = FALSE;

    if ((worker) && (worker->smpw_MasterPort))
    {
        worker->smpw_Node.ln_Type = 0;

        worker->smpw_MsgPort = CreateMsgPort();

        workPrivate = AllocMem(sizeof(struct WorkersWork) + (worker->smpw_MaxWork * sizeof(complexno_t)), MEMF_CLEAR|MEMF_ANY);
        
        if (workPrivate)
        {
            workPrivate->workMax = worker->smpw_MaxWork;
            workPrivate->workOversamp = worker->smpw_Oversample;
            workPrivate->workOver2 = workPrivate->workOversamp * workPrivate->workOversamp;
            workPrivate->lock = worker->smpw_Lock;

            while (doWork)
            {
                ULONG workWidth, workHeight, workStart, workEnd;
                ULONG *workBuffer;
                ULONG workType;

                /* we are ready for work .. */
                worker->smpw_Node.ln_Type = 1;
                Signal(worker->smpw_SyncTask, SIGBREAKF_CTRL_C);
                WaitPort(worker->smpw_MsgPort);

                while((workMsg = (struct SMPWorkMessage *) GetMsg(worker->smpw_MsgPort)))
                {
                    buddha = FALSE;

                    /* cache the requested work and free the message ... */
                    workType = workMsg->smpwm_Type;
                    workBuffer = workMsg->smpwm_Buffer;
                    workWidth = workMsg->smpwm_Width;
                    workHeight = workMsg->smpwm_Height;
                    workStart = workMsg->smpwm_Start;
                    workEnd = workMsg->smpwm_End;

                    FreeMem(workMsg, sizeof(struct SMPWorkMessage));
                    
                    /* now process it .. */
                    switch (workType)
                    {
                        case SPMWORKTYPE_FINISHED:
                            doWork = FALSE;
                            break;
                        case SPMWORKTYPE_BUDDHA:
                            buddha = TRUE;
                        case SPMWORKTYPE_MANDLEBROT: // fallthrough
                            processWork(workPrivate, workBuffer, workWidth, workHeight, workStart, workEnd, buddha);

                            /* let our "parent" know we are done .. */
                            Signal(worker->smpw_MasterPort->mp_SigTask, SIGBREAKF_CTRL_D);
                            break;
                    }
                }
            }
            FreeMem(workPrivate, sizeof(struct WorkersWork) + (workPrivate->workMax * sizeof(complexno_t)));
        }
        Remove(&worker->smpw_Node);
        DeleteMsgPort(worker->smpw_MsgPort);
        Signal(worker->smpw_MasterPort->mp_SigTask, SIGBREAKF_CTRL_C);
    }
}