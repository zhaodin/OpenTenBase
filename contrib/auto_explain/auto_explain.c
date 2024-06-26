/*-------------------------------------------------------------------------
 *
 * auto_explain.c
 *
 *
 * Copyright (c) 2008-2017, PostgreSQL Global Development Group
 *
 * This source code file contains modifications made by THL A29 Limited ("Tencent Modifications").
 * All Tencent Modifications are Copyright (C) 2023 THL A29 Limited.
 * 
 * IDENTIFICATION
 *      contrib/auto_explain/auto_explain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "commands/explain.h"
#include "executor/instrument.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

/* GUC variables */
/* 自动执行计划日志记录的最短执行时间（毫秒），-1表示禁用 */
static int    auto_explain_log_min_duration = -1; /* msec or -1 */
/* 是否使用EXPLAIN ANALYZE记录执行计划 */
static bool auto_explain_log_analyze = false;
/* 是否记录详细信息 */
static bool auto_explain_log_verbose = false;
/* 是否记录缓冲区信息 */
static bool auto_explain_log_buffers = false;
/* 是否记录触发器信息 */
static bool auto_explain_log_triggers = false;
/* 是否记录计时信息 */
static bool auto_explain_log_timing = true;
/* 记录格式 */
static int    auto_explain_log_format = EXPLAIN_FORMAT_TEXT;
/* 是否记录嵌套语句 */
static bool auto_explain_log_nested_statements = false;
/* 执行计划采样率 */
static double auto_explain_sample_rate = 1;

/* 计划格式选项 */
static const struct config_enum_entry format_options[] = {
    {"text", EXPLAIN_FORMAT_TEXT, false},
    {"xml", EXPLAIN_FORMAT_XML, false},
    {"json", EXPLAIN_FORMAT_JSON, false},
    {"yaml", EXPLAIN_FORMAT_YAML, false},
    {NULL, 0, false}
};

/* Current nesting depth of ExecutorRun calls */
/* ExecutorRun调用的当前嵌套深度 */
static int    nesting_level = 0;

/* Saved hook values in case of unload */
/* 卸载时保存的钩子值 */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/* Is the current query sampled, per backend */
/* 当前查询是否采样 */
static bool current_query_sampled = true;

/* 检查自动执行计划是否启用 */
#define auto_explain_enabled() \
    (auto_explain_log_min_duration >= 0 && \
     (nesting_level == 0 || auto_explain_log_nested_statements))

void        _PG_init(void);
void        _PG_fini(void);

/* 执行计划钩子函数 */
static void explain_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void explain_ExecutorRun(QueryDesc *queryDesc,
                    ScanDirection direction,
                    uint64 count, bool execute_once);
static void explain_ExecutorFinish(QueryDesc *queryDesc);
static void explain_ExecutorEnd(QueryDesc *queryDesc);


/*
 * Module load callback
 */
 /*
  * 模块加载回调
  */
void
_PG_init(void)
{
    /* Define custom GUC variables. */
    DefineCustomIntVariable("auto_explain.log_min_duration",
                            "Sets the minimum execution time above which plans will be logged.",
                            "Zero prints all plans. -1 turns this feature off.",
                            &auto_explain_log_min_duration,
                            -1,
                            -1, INT_MAX / 1000,
                            PGC_SUSET,
                            GUC_UNIT_MS,
                            NULL,
                            NULL,
                            NULL);

    DefineCustomBoolVariable("auto_explain.log_analyze",
                             "Use EXPLAIN ANALYZE for plan logging.",
                             NULL,
                             &auto_explain_log_analyze,
                             false,
                             PGC_SUSET,
                             0,
                             NULL,
                             NULL,
                             NULL);

    DefineCustomBoolVariable("auto_explain.log_verbose",
                             "Use EXPLAIN VERBOSE for plan logging.",
                             NULL,
                             &auto_explain_log_verbose,
                             false,
                             PGC_SUSET,
                             0,
                             NULL,
                             NULL,
                             NULL);

    DefineCustomBoolVariable("auto_explain.log_buffers",
                             "Log buffers usage.",
                             NULL,
                             &auto_explain_log_buffers,
                             false,
                             PGC_SUSET,
                             0,
                             NULL,
                             NULL,
                             NULL);

    DefineCustomBoolVariable("auto_explain.log_triggers",
                             "Include trigger statistics in plans.",
                             "This has no effect unless log_analyze is also set.",
                             &auto_explain_log_triggers,
                             false,
                             PGC_SUSET,
                             0,
                             NULL,
                             NULL,
                             NULL);

    DefineCustomEnumVariable("auto_explain.log_format",
                             "EXPLAIN format to be used for plan logging.",
                             NULL,
                             &auto_explain_log_format,
                             EXPLAIN_FORMAT_TEXT,
                             format_options,
                             PGC_SUSET,
                             0,
                             NULL,
                             NULL,
                             NULL);

    DefineCustomBoolVariable("auto_explain.log_nested_statements",
                             "Log nested statements.",
                             NULL,
                             &auto_explain_log_nested_statements,
                             false,
                             PGC_SUSET,
                             0,
                             NULL,
                             NULL,
                             NULL);

    DefineCustomBoolVariable("auto_explain.log_timing",
                             "Collect timing data, not just row counts.",
                             NULL,
                             &auto_explain_log_timing,
                             true,
                             PGC_SUSET,
                             0,
                             NULL,
                             NULL,
                             NULL);

    DefineCustomRealVariable("auto_explain.sample_rate",
                             "Fraction of queries to process.",
                             NULL,
                             &auto_explain_sample_rate,
                             1.0,
                             0.0,
                             1.0,
                             PGC_SUSET,
                             0,
                             NULL,
                             NULL,
                             NULL);

    EmitWarningsOnPlaceholders("auto_explain");

    /* Install hooks. */
    prev_ExecutorStart = ExecutorStart_hook;
    ExecutorStart_hook = explain_ExecutorStart;
    prev_ExecutorRun = ExecutorRun_hook;
    ExecutorRun_hook = explain_ExecutorRun;
    prev_ExecutorFinish = ExecutorFinish_hook;
    ExecutorFinish_hook = explain_ExecutorFinish;
    prev_ExecutorEnd = ExecutorEnd_hook;
    ExecutorEnd_hook = explain_ExecutorEnd;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
    /* Uninstall hooks. */
    ExecutorStart_hook = prev_ExecutorStart;
    ExecutorRun_hook = prev_ExecutorRun;
    ExecutorFinish_hook = prev_ExecutorFinish;
    ExecutorEnd_hook = prev_ExecutorEnd;
}

/*
 * ExecutorStart hook: start up logging if needed
 */
static void
explain_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    /*
     * For rate sampling, randomly choose top-level statement. Either all
     * nested statements will be explained or none will.
     */
    if (auto_explain_log_min_duration >= 0 && nesting_level == 0)
        current_query_sampled = (random() < auto_explain_sample_rate *
                                 MAX_RANDOM_VALUE);

    if (auto_explain_enabled() && current_query_sampled)
    {
        /* Enable per-node instrumentation iff log_analyze is required. */
        if (auto_explain_log_analyze && (eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
        {
            if (auto_explain_log_timing)
                queryDesc->instrument_options |= INSTRUMENT_TIMER;
            else
                queryDesc->instrument_options |= INSTRUMENT_ROWS;
            if (auto_explain_log_buffers)
                queryDesc->instrument_options |= INSTRUMENT_BUFFERS;
        }
    }

    if (prev_ExecutorStart)
        prev_ExecutorStart(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);

    if (auto_explain_enabled() && current_query_sampled)
    {
        /*
         * Set up to track total elapsed time in ExecutorRun.  Make sure the
         * space is allocated in the per-query context so it will go away at
         * ExecutorEnd.
         */
        if (queryDesc->totaltime == NULL)
        {
            MemoryContext oldcxt;

            oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
            queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL);
            MemoryContextSwitchTo(oldcxt);
        }
    }
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
explain_ExecutorRun(QueryDesc* queryDesc, ScanDirection direction,
    uint64 count, bool execute_once)
{
    nesting_level++; // 增加嵌套深度
    PG_TRY();
    {
        if (prev_ExecutorRun)
            prev_ExecutorRun(queryDesc, direction, count, execute_once); // 调用原始 ExecutorRun 函数
        else
            standard_ExecutorRun(queryDesc, direction, count, execute_once); // 使用标准 ExecutorRun 函数
        nesting_level--; // 减少嵌套深度
    }
    PG_CATCH();
    {
        nesting_level--; // 减少嵌套深度
        PG_RE_THROW(); // 重新抛出异常
    }
    PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
explain_ExecutorFinish(QueryDesc* queryDesc)
{
    nesting_level++; // 增加嵌套深度
    PG_TRY();
    {
        if (prev_ExecutorFinish)
            prev_ExecutorFinish(queryDesc); // 调用原始 ExecutorFinish 函数
        else
            standard_ExecutorFinish(queryDesc); // 使用标准 ExecutorFinish 函数
        nesting_level--; // 减少嵌套深度
    }
    PG_CATCH();
    {
        nesting_level--; // 减少嵌套深度
        PG_RE_THROW(); // 重新抛出异常
    }
    PG_END_TRY();
}

/*
 * ExecutorEnd hook: log results if needed
 */
static void
explain_ExecutorEnd(QueryDesc* queryDesc)
{
    if (queryDesc->totaltime && auto_explain_enabled() && current_query_sampled)
    {
        double        msec;

        /*
         * Make sure stats accumulation is done.  (Note: it's okay if several
         * levels of hook all do this.)
         */
        InstrEndLoop(queryDesc->totaltime); // 结束循环并统计执行时间

        /* Log plan if duration is exceeded. */
        msec = queryDesc->totaltime->total * 1000.0; // 将执行时间转换为毫秒
        if (msec >= auto_explain_log_min_duration) // 如果执行时间超过设定的最小记录时间
        {
            ExplainState* es = NewExplainState(); // 创建 ExplainState 结构体

            es->analyze = (queryDesc->instrument_options && auto_explain_log_analyze); // 是否分析执行计划
            es->verbose = auto_explain_log_verbose; // 是否记录详细信息
            es->buffers = (es->analyze && auto_explain_log_buffers); // 是否记录缓冲区信息
            es->timing = (es->analyze && auto_explain_log_timing); // 是否记录计时信息
            es->summary = es->analyze; // 是否总结执行计划
            es->format = auto_explain_log_format; // 记录格式

            ExplainBeginOutput(es); // 开始记录输出
            ExplainQueryText(es, queryDesc); // 记录查询文本
            ExplainPrintPlan(es, queryDesc); // 记录执行计划
            if (es->analyze && auto_explain_log_triggers)
                ExplainPrintTriggers(es, queryDesc); // 记录触发器信息
            ExplainEndOutput(es); // 结束记录输出

            /* Remove last line break */
            if (es->str->len > 0 && es->str->data[es->str->len - 1] == '\n')
                es->str->data[--es->str->len] = '\0'; // 移除最后一个换行符

            /* Fix JSON to output an object */
            if (auto_explain_log_format == EXPLAIN_FORMAT_JSON)
            {
                es->str->data[0] = '{';
                es->str->data[es->str->len - 1] = '}';
            }

            /*
             * Note: we rely on the existing logging of context or
             * debug_query_string to identify just which statement is being
             * reported.  This isn't ideal but trying to do it here would
             * often result in duplication.
             */
            ereport(LOG,
                (errmsg("duration: %.3f ms  plan:\n%s",
                    msec, es->str->data),
                    errhidestmt(true))); // 记录执行计划到日志中

            pfree(es->str->data); // 释放 ExplainState 结构体中的字符串数据
        }
    }

    if (prev_ExecutorEnd)
        prev_ExecutorEnd(queryDesc); // 调用原始 ExecutorEnd 函数
    else
        standard_ExecutorEnd(queryDesc); // 使用标准 ExecutorEnd 函数
}
