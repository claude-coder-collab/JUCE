/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-9-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

#pragma once

/** @cond */
namespace juce
{

/*  Holds parameter changes which a host reports ahead of the time at which
    they should take effect, and hands them out once the audio callback has
    caught up.

    Changes are collected with set() and grouped into a numbered state by
    commit(). When the audio callback learns which state applies to the block
    that it's about to render, it can use applyNextUpdate() to retrieve all of
    the changes belonging to that state and to any earlier states.

    set(), commit() and cancel() may be called from any non-realtime thread.
    applyNextUpdate() doesn't allocate, and with Blocking::no it never waits
    or makes system calls, so it can be called from the audio callback.
*/
class DeferredParameterUpdates
{
public:
    struct Update
    {
        size_t parameterIndex;
        float value;
    };

    enum class Blocking
    {
        no,
        yes
    };

    /*  The capacity is the largest number of changes that can be waiting for
        the audio callback at any time. It must not be smaller than the number
        of parameters.
    */
    DeferredParameterUpdates (size_t numParameters, size_t capacity)
        : parameters (numParameters),
          queue (capacity + 1),
          fifo ((int) queue.size())
    {
        jassert (numParameters <= capacity);
        dirtyParameters.reserve (numParameters);
    }

    size_t getNumParameters() const noexcept { return parameters.size(); }

    /*  Records a new value for a parameter. The value will be included in the
        next state that is created by commit().
    */
    void set (size_t parameterIndex, float value)
    {
        jassert (parameterIndex < parameters.size());

        const ScopedLock lock (modelLock);

        auto& parameter = parameters[parameterIndex];
        parameter.latestValue = value;
        ++parameter.issued;

        if (! std::exchange (parameter.dirty, true))
            dirtyParameters.push_back (parameterIndex);
    }

    /*  Groups all of the values that were set since the last call into a new
        state, which becomes the result of getLatestState().

        Returns false, leaving everything unchanged, if there isn't enough
        space to store the new state. In that case, some updates need to be
        applied before trying again.
    */
    bool commit()
    {
        const ScopedLock lock (modelLock);

        if (dirtyParameters.empty())
            return true;

        const auto numToWrite = (int) dirtyParameters.size();

        if (fifo.getFreeSpace() < numToWrite)
            return false;

        const auto state = latestState.load (std::memory_order_relaxed) + 1;

        {
            auto dirty = dirtyParameters.cbegin();

            fifo.write (numToWrite).forEach ([&] (int queueIndex)
            {
                const auto parameterIndex = *dirty++;
                auto& parameter = parameters[parameterIndex];
                parameter.dirty = false;

                queue[(size_t) queueIndex] = { state, parameter.issued, (uint32_t) parameterIndex, parameter.latestValue };
            });
        }

        dirtyParameters.clear();
        latestState.store (state, std::memory_order_relaxed);

        return true;
    }

    /*  Returns the number of the state that was most recently created by
        commit(). The first state has the number 1.
    */
    int64_t getLatestState() const noexcept
    {
        return latestState.load (std::memory_order_relaxed);
    }

    /*  Discards any values for this parameter that haven't been applied yet.

        Once this function returns, applyNextUpdate() won't apply any value
        for this parameter that was set before the call, so the caller can
        safely give the parameter a new value directly.
    */
    void cancel (size_t parameterIndex)
    {
        jassert (parameterIndex < parameters.size());

        const ScopedLock lock (modelLock);

        auto& parameter = parameters[parameterIndex];

        if (std::exchange (parameter.dirty, false))
            dirtyParameters.erase (std::remove (dirtyParameters.begin(), dirtyParameters.end(), parameterIndex),
                                   dirtyParameters.end());

        // Waits for any update that is being applied, so that none can be applied after this
        const SpinLock::ScopedLockType applying (applyLock);
        parameter.cancelled.store (parameter.issued, std::memory_order_relaxed);
    }

    /*  If a value has been set for this parameter but hasn't been applied or
        cancelled yet, returns the most recent such value.
    */
    std::optional<float> getPendingValue (size_t parameterIndex) const
    {
        jassert (parameterIndex < parameters.size());

        const ScopedLock lock (modelLock);

        const auto& parameter = parameters[parameterIndex];
        const auto handled = jmax (parameter.applied.load (std::memory_order_relaxed),
                                   parameter.cancelled.load (std::memory_order_relaxed));

        if (handled < parameter.issued)
            return parameter.latestValue;

        return {};
    }

    /*  Returns true if any values are waiting to be committed or applied. */
    bool hasPendingUpdates() const
    {
        const ScopedLock lock (modelLock);
        return ! dirtyParameters.empty() || fifo.getNumReady() > 0;
    }

    /*  Finds the oldest update belonging to the specified state, or to any
        earlier state, and passes its parameter index and value to setValue.
        Returns the update that was applied, or nullopt if there are no more
        updates for this state.

        With Blocking::no, this will also return nullopt if another thread is
        currently applying or cancelling updates. The remaining updates will
        be returned by a later call.

        While setValue runs, other threads can't apply or cancel updates, so it
        should do as little as possible and must not call back into this
        object. Anything else that needs to happen in response to the update
        can be done using the returned value.
    */
    template <typename SetValue>
    std::optional<Update> applyNextUpdate (int64_t state, Blocking blocking, SetValue&& setValue)
    {
        if (blocking == Blocking::yes)
            applyLock.enter();
        else if (! applyLock.tryEnter())
            return {};

        const ScopeGuard unlock { [this] { applyLock.exit(); } };

        for (;;)
        {
            int start1, size1, start2, size2;
            fifo.prepareToRead (1, start1, size1, start2, size2);

            if (size1 + size2 == 0)
                return {};

            const auto entry = queue[(size_t) (size1 != 0 ? start1 : start2)];

            if (state < entry.state)
                return {};

            fifo.finishedRead (1);

            auto& parameter = parameters[entry.parameterIndex];

            if (entry.sequence <= parameter.cancelled.load (std::memory_order_relaxed))
                continue;

            setValue ((size_t) entry.parameterIndex, entry.value);
            parameter.applied.store (entry.sequence, std::memory_order_relaxed);

            return Update { entry.parameterIndex, entry.value };
        }
    }

private:
    struct Parameter
    {
        float latestValue = 0.0f;
        uint64_t issued = 0;
        bool dirty = false;
        std::atomic<uint64_t> applied { 0 }, cancelled { 0 };
    };

    struct Entry
    {
        int64_t state;
        uint64_t sequence;
        uint32_t parameterIndex;
        float value;
    };

    std::vector<Parameter> parameters;
    std::vector<size_t> dirtyParameters;
    std::vector<Entry> queue;
    AbstractFifo fifo;
    std::atomic<int64_t> latestState { 0 };
    CriticalSection modelLock;
    SpinLock applyLock;
};

} // namespace juce
/** @endcond */
