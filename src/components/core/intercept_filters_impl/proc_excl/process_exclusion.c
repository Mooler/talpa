/*
 * process_exclusion.c
 *
 * TALPA Filesystem Interceptor
 *
 * Copyright (C) 2004 Sophos Plc, Oxford, England.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License Version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program; if not,
 * write to the Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <linux/kernel.h>

#include <linux/slab.h>
#include <linux/string.h>

#define TALPA_SUBSYS "procexcl"
#include "common/talpa.h"
#include "process_exclusion.h"

/*
 * Forward declare implementation methods.
 */
static void examineFile(const void* self, IEvaluationReport* report, const IPersonality* userInfo, const IFileInfo* info, IFile* file);
static void examineFilesystem(const void* self, IEvaluationReport* report, const IPersonality* userInfo, const IFilesystemInfo* info);

static ProcessExcluded* registerProcess(void* self);
static void deregisterProcess(void* self, ProcessExcluded* obj);
static ProcessExcluded* active(void* self, ProcessExcluded* obj);
static ProcessExcluded* idle(void* self, ProcessExcluded* obj);

static bool enable(void* self);
static void disable(void* self);
static bool isEnabled(const void* self);
static const char* configName(const void* self);
static const PODConfigurationElement* allConfig(const void* self);
static const char* config(const void* self, const char* name);
static void setConfig(void* self, const char* name, const char* value);

static void deleteProcessExclusionProcessor(struct tag_ProcessExclusionProcessor* object);


/*
 * Constants
 */
#define CFG_STATUS          "status"

#define CFG_VALUE_ENABLED   "enabled"
#define CFG_VALUE_DISABLED  "disabled"
#define CFG_ACTION_ENABLE   "enable"
#define CFG_ACTION_DISABLE  "disable"

/*
 * Template Object.
 */
static ProcessExclusionProcessor template_ProcessExclusionProcessor =
    {
        {
            examineFile,
            examineFilesystem,
            enable,
            disable,
            isEnabled,
            0,
            (void (*)(void*))deleteProcessExclusionProcessor
        },
        {
            registerProcess,
            deregisterProcess,
            active,
            idle,
            0,
            (void (*)(void*))deleteProcessExclusionProcessor
        },
        {
            configName,
            allConfig,
            config,
            setConfig,
            0,
            (void (*)(void*))deleteProcessExclusionProcessor
        },
        deleteProcessExclusionProcessor,
        TALPA_MUTEX_INIT,
        true,
        TALPA_RCU_UNLOCKED,
        { },
        {
            {0, 0, PROCEXCL_CFGDATASIZE, true, true },
            {0, 0, 0, false, false }
        },
        {
            { CFG_STATUS, CFG_VALUE_ENABLED }
        },
    };
#define this    ((ProcessExclusionProcessor*)self)



/*
 * Object creation/destruction.
 */
ProcessExclusionProcessor* newProcessExclusionProcessor(void)
{
    ProcessExclusionProcessor* object;


    object = kmalloc(sizeof(template_ProcessExclusionProcessor), SLAB_KERNEL);
    if (object != 0)
    {
        dbg("object at 0x%p", object);
        memcpy(object, &template_ProcessExclusionProcessor, sizeof(template_ProcessExclusionProcessor));
        object->i_IInterceptFilter.object =
            object->i_IProcessExcluder.object =
            object->i_IConfigurable.object = object;

        talpa_mutex_init(&object->mConfigSerialize);
        talpa_rcu_lock_init(&object->mExcludedLock);
        TALPA_INIT_LIST_HEAD(&object->mExcluded);

        object->mConfig[0].name  = object->mStateConfigData[0].name;
        object->mConfig[0].value = object->mStateConfigData[0].value;
    }
    return object;
}

static void deleteProcessExclusionProcessor(struct tag_ProcessExclusionProcessor* object)
{
    ProcessExcluded* process;
    struct list_head* excluded;
    struct list_head* iter;


    /* Cleanup registered processs */
    talpa_rcu_write_lock(&object->mExcludedLock);
    talpa_list_for_each_safe_rcu(excluded, iter, &object->mExcluded)
    {
        process = talpa_list_entry(excluded, ProcessExcluded, head);
        talpa_list_del_rcu(excluded);
        kfree(process);
    }
    talpa_rcu_write_unlock(&object->mExcludedLock);
    talpa_rcu_synchronize();

    kfree(object);
    return;
}

static inline bool findProcessExcluded(const void* self, const ProcessExcluded* process)
{
    ProcessExcluded* excluded;

    talpa_rcu_read_lock(&this->mExcludedLock);
    talpa_list_for_each_entry_rcu(excluded, &this->mExcluded, head)
    {
        if ( excluded == process )
        {
            talpa_rcu_read_unlock(&this->mExcludedLock);
            return true;
        }
    }
    talpa_rcu_read_unlock(&this->mExcludedLock);

    return false;
}

static inline bool checkProcessExcluded(const void* self)
{
    ProcessExcluded* excluded;
    void* unique = current->files;

    talpa_rcu_read_lock(&this->mExcludedLock);
    talpa_list_for_each_entry_rcu(excluded, &this->mExcluded, head)
    {
        if ( excluded->unique == unique )
        {
            talpa_rcu_read_unlock(&this->mExcludedLock);
            return excluded->active;
        }
    }
    talpa_rcu_read_unlock(&this->mExcludedLock);

    return false;
}

/*
 * IInterceptFilter.
 */
static void examineFile(const void* self, IEvaluationReport* report, const IPersonality* userInfo, const IFileInfo* info, IFile* file)
{
    if ( checkProcessExcluded(this) )
    {
        dbg("[intercepted %u-%u-%u] %s - excluded", processParentPID(current), current->tgid, current->pid, current->comm);
        report->setRecommendedAction(report, EIA_Allow);
    }

    return;
}

static void examineFilesystem(const void* self, IEvaluationReport* report,
                                const IPersonality* userInfo,
                                const IFilesystemInfo* info)
{
    if ( checkProcessExcluded(this) )
    {
        dbg("[intercepted %u-%u-%u] %s - excluded", processParentPID(current), current->tgid, current->pid, current->comm);
        report->setRecommendedAction(report, EIA_Allow);
    }

    return;
}

/*
 * IProcessExcluder.
 */
static ProcessExcluded* registerProcess(void* self)
{
    ProcessExcluded* process;

    process = kmalloc(sizeof(ProcessExcluded), GFP_KERNEL);

    if ( !process )
    {
        err("Failed to allocate process!");
        return NULL;
    }

    TALPA_INIT_LIST_HEAD(&process->head);
    process->unique = current->files;
    process->processID = current->tgid;
    process->threadID = current->pid;
    process->active = false;

    talpa_rcu_write_lock(&this->mExcludedLock);
    talpa_list_add_tail_rcu(&process->head, &this->mExcluded);
    talpa_rcu_write_unlock(&this->mExcludedLock);

    dbg("Process [%u/%u] registered", process->processID, process->threadID);
    return process;
}

static void deregisterProcess(void* self, ProcessExcluded* obj)
{
    /* Check if the client was present before the core
       was loaded. It can happen on core hot swap */
    if ( !findProcessExcluded(this, obj) )
    {
        dbg("Isolated process [%u/%u] deregistred", current->tgid, current->pid);
        return;
    }

    talpa_rcu_write_lock(&this->mExcludedLock);
    talpa_list_del_rcu(&obj->head);
    talpa_rcu_write_unlock(&this->mExcludedLock);
    dbg("Process [%u/%u] deregistered", obj->processID, obj->threadID);
    talpa_rcu_synchronize();
    kfree(obj);

    return;
}

static ProcessExcluded* active(void* self, ProcessExcluded* obj)
{
    if ( !findProcessExcluded(this, obj) )
    {
        dbg("Process implicitly registering...");
        obj = registerProcess(this);
    }

    obj->active = true;
    dbg("Process [%u-%u] Active", obj->processID, obj->threadID);

    return obj;
}

static ProcessExcluded* idle(void* self, ProcessExcluded* obj)
{
    if ( !findProcessExcluded(this, obj) )
    {
        dbg("Process implicitly registering...");
        obj = registerProcess(this);
    }

    obj->active = false;
    dbg("Process [%u-%u] Idle", obj->processID, obj->threadID);

    return obj;
}


/*
 * Internal configuration.
 */

static bool enable(void* self)
{
    talpa_mutex_lock(&this->mConfigSerialize);
    if (!this->mEnabled)
    {
        this->mEnabled = true;
        strcpy(this->mConfig[0].value, CFG_VALUE_ENABLED);
        info("Enabled");
    }
    talpa_mutex_unlock(&this->mConfigSerialize);
    return true;
}

static void disable(void* self)
{
    talpa_mutex_lock(&this->mConfigSerialize);
    if (this->mEnabled)
    {
        this->mEnabled = false;
        strcpy(this->mConfig[0].value, CFG_VALUE_DISABLED);
        info("Disabled");
    }
    talpa_mutex_unlock(&this->mConfigSerialize);
    return;
}

static bool isEnabled(const void* self)
{
    return this->mEnabled;
}

/*
 * IConfigurable.
 */
static const char* configName(const void* self)
{
    return "ProcessExclusionProcessor";
}

static const PODConfigurationElement* allConfig(const void* self)
{
    return this->mConfig;
}

static const char* config(const void* self, const char* name)
{
    PODConfigurationElement*    cfgElement;


    /*
     * Find the named item.
     */
    for (cfgElement = this->mConfig; cfgElement->name != 0; cfgElement++)
    {
        if (strcmp(name, cfgElement->name) == 0)
        {
            break;
        }
    }

    /*
     * Return what was found else a null pointer.
     */
    if (cfgElement->name != 0)
    {
        return cfgElement->value;
    }
    return 0;
}

static void  setConfig(void* self, const char* name, const char* value)
{
   PODConfigurationElement*    cfgElement;


    /*
     * Find the named item.
     */
    for (cfgElement = this->mConfig; cfgElement->name != 0; cfgElement++)
    {
        if (strcmp(name, cfgElement->name) == 0)
        {
            break;
        }
    }

    /*
     * Cant set that which does not exist!
     */
    if (cfgElement->name == 0)
    {
        return;
    }

    /*
     * OK time to do some work...
     */
    if (strcmp(name, CFG_STATUS) == 0)
    {
        if (strcmp(value, CFG_ACTION_ENABLE) == 0)
        {
            enable(this);
        }
        else if (strcmp(value, CFG_ACTION_DISABLE) == 0)
        {
            disable(this);
        }
    }

    return;
}

/*
 * End of process_exclusion.c
 */

