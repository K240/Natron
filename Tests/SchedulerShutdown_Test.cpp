// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/GenericSchedulerThread.h"
#include <QSemaphore>
#include <gtest/gtest.h>

NATRON_NAMESPACE_USING
namespace {
class ShutdownScheduler : public GenericSchedulerThread {
public:
    int drains=0,quits=0;
    bool delayed=false;
    QSemaphore entered,releaseWork;
    ~ShutdownScheduler() override {releaseWork.release();quitThread(false);waitForThreadToQuit_enforce_blocking();}
private:
    TaskQueueBehaviorEnum tasksQueueBehaviour() const override {return eTaskQueueBehaviorProcessInOrder;}
    ThreadStateEnum threadLoopOnce(const GenericThreadStartArgsPtr&) override {
        if(delayed){entered.release();releaseWork.acquire();return resolveState();}
        return eThreadStateStopped;
    }
    void onQuitRequested(bool) override {++quits;}
    void onWaitForThreadToQuit() override {++drains;}
};
}

TEST(SchedulerShutdown, PermanentQuitBeforeFirstTask) {
    ShutdownScheduler scheduler;
    EXPECT_FALSE(scheduler.quitThread(false));
    EXPECT_EQ(1,scheduler.quits);
    EXPECT_FALSE(scheduler.startTask(std::make_shared<GenericThreadStartArgs>()));
    EXPECT_TRUE(scheduler.waitForThreadToQuit_enforce_blocking());
    EXPECT_EQ(1,scheduler.drains);
}

TEST(SchedulerShutdown, DrainChildrenAfterWorkerStopped) {
    ShutdownScheduler scheduler;
    ASSERT_TRUE(scheduler.startTask(std::make_shared<GenericThreadStartArgs>()));
    ASSERT_TRUE(scheduler.wait(5000));
    ASSERT_FALSE(scheduler.isRunning());
    EXPECT_TRUE(scheduler.waitForThreadToQuit_enforce_blocking());
    EXPECT_EQ(1,scheduler.drains);
    EXPECT_FALSE(scheduler.quitThread(false));
    EXPECT_FALSE(scheduler.startTask(std::make_shared<GenericThreadStartArgs>()));
}

TEST(SchedulerShutdown, TemporaryQuitStillAllowsRestart) {
    ShutdownScheduler scheduler;
    EXPECT_FALSE(scheduler.quitThread(true));
    ASSERT_TRUE(scheduler.startTask(std::make_shared<GenericThreadStartArgs>()));
    ASSERT_TRUE(scheduler.wait(5000));
    EXPECT_TRUE(scheduler.waitForThreadToQuit_enforce_blocking());
    ASSERT_TRUE(scheduler.startTask(std::make_shared<GenericThreadStartArgs>()));
    ASSERT_TRUE(scheduler.wait(5000));
}

TEST(SchedulerShutdown, PermanentQuitSupersedesPendingTemporaryQuit) {
    ShutdownScheduler scheduler;scheduler.delayed=true;
    ASSERT_TRUE(scheduler.startTask(std::make_shared<GenericThreadStartArgs>()));
    ASSERT_TRUE(scheduler.entered.tryAcquire(1,5000));
    EXPECT_TRUE(scheduler.quitThread(true));
    EXPECT_TRUE(scheduler.quitThread(false));
    scheduler.releaseWork.release();
    ASSERT_TRUE(scheduler.wait(5000));
    EXPECT_TRUE(scheduler.waitForThreadToQuit_enforce_blocking());
    EXPECT_FALSE(scheduler.startTask(std::make_shared<GenericThreadStartArgs>()));
}
