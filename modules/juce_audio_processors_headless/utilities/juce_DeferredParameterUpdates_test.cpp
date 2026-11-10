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

namespace juce
{

class DeferredParameterUpdatesTests final : public UnitTest
{
public:
    DeferredParameterUpdatesTests()
        : UnitTest ("DeferredParameterUpdates", UnitTestCategories::audioProcessors)
    {}

    void runTest() override
    {
        using Blocking = DeferredParameterUpdates::Blocking;
        using Updates = std::vector<std::pair<size_t, float>>;

        const auto applyAll = [] (DeferredParameterUpdates& updates, int64_t state, Blocking blocking = Blocking::no)
        {
            Updates result;

            while (const auto update = updates.applyNextUpdate (state, blocking, [] (size_t, float) {}))
                result.emplace_back (update->parameterIndex, update->value);

            return result;
        };

        beginTest ("Values are not available until they have been committed");
        {
            DeferredParameterUpdates updates { 4, 16 };
            expect (! updates.hasPendingUpdates());
            expect (updates.getLatestState() == 0);

            updates.set (2, 0.5f);
            expect (updates.hasPendingUpdates());
            expect (applyAll (updates, std::numeric_limits<int64_t>::max()).empty());

            expect (updates.commit());
            expect (updates.getLatestState() == 1);
            expect (applyAll (updates, 1) == Updates { { 2, 0.5f } });
            expect (! updates.hasPendingUpdates());
        }

        beginTest ("Updates are held back until their state is reached");
        {
            DeferredParameterUpdates updates { 4, 16 };

            updates.set (0, 0.1f);
            updates.set (1, 0.2f);
            expect (updates.commit());

            updates.set (0, 0.3f);
            expect (updates.commit());

            updates.set (3, 0.4f);
            expect (updates.commit());
            expect (updates.getLatestState() == 3);

            expect (applyAll (updates, 0).empty());
            expect (applyAll (updates, 1) == Updates { { 0, 0.1f }, { 1, 0.2f } });
            expect (applyAll (updates, 1).empty());
            expect (applyAll (updates, 3) == Updates { { 0, 0.3f }, { 3, 0.4f } });
            expect (! updates.hasPendingUpdates());
        }

        beginTest ("Only the last value set before a commit is kept");
        {
            DeferredParameterUpdates updates { 2, 16 };

            updates.set (1, 0.1f);
            updates.set (1, 0.2f);
            updates.set (1, 0.3f);
            expect (updates.commit());

            expect (applyAll (updates, 1) == Updates { { 1, 0.3f } });
        }

        beginTest ("Committing without changes keeps the current state");
        {
            DeferredParameterUpdates updates { 2, 16 };

            expect (updates.commit());
            expect (updates.getLatestState() == 0);

            updates.set (0, 1.0f);
            expect (updates.commit());
            expect (updates.commit());
            expect (updates.getLatestState() == 1);
        }

        beginTest ("The value passed to the callback matches the returned update");
        {
            DeferredParameterUpdates updates { 2, 16 };

            updates.set (1, 0.75f);
            expect (updates.commit());

            Updates applied;
            const auto update = updates.applyNextUpdate (1, Blocking::no, [&] (size_t index, float value)
            {
                applied.emplace_back (index, value);
            });

            expect (update.has_value());
            expect (applied == Updates { { 1, 0.75f } });
            expect (applied == Updates { { update->parameterIndex, update->value } });
        }

        beginTest ("Pending values are reported until they are applied");
        {
            DeferredParameterUpdates updates { 2, 16 };
            expect (! updates.getPendingValue (0).has_value());

            updates.set (0, 0.25f);
            expect (updates.getPendingValue (0) == std::optional<float> (0.25f));
            expect (! updates.getPendingValue (1).has_value());

            expect (updates.commit());
            updates.set (0, 0.5f);
            expect (updates.commit());
            expect (updates.getPendingValue (0) == std::optional<float> (0.5f));

            expect (applyAll (updates, 1).size() == 1);
            expect (updates.getPendingValue (0) == std::optional<float> (0.5f));

            expect (applyAll (updates, 2).size() == 1);
            expect (! updates.getPendingValue (0).has_value());
        }

        beginTest ("Cancelling discards committed and uncommitted values");
        {
            DeferredParameterUpdates updates { 3, 16 };

            updates.set (0, 0.1f);
            updates.set (1, 0.2f);
            expect (updates.commit());

            updates.set (0, 0.3f);
            updates.set (2, 0.4f);

            updates.cancel (0);
            expect (! updates.getPendingValue (0).has_value());

            expect (updates.commit());
            expect (applyAll (updates, updates.getLatestState()) == Updates { { 1, 0.2f }, { 2, 0.4f } });
            expect (! updates.hasPendingUpdates());
        }

        beginTest ("Values set after cancelling are still delivered");
        {
            DeferredParameterUpdates updates { 2, 16 };

            updates.set (0, 0.1f);
            expect (updates.commit());
            updates.cancel (0);

            updates.set (0, 0.2f);
            expect (updates.getPendingValue (0) == std::optional<float> (0.2f));
            expect (updates.commit());

            expect (applyAll (updates, updates.getLatestState()) == Updates { { 0, 0.2f } });
        }

        beginTest ("Commit fails without losing values when there is no space");
        {
            DeferredParameterUpdates updates { 2, 3 };

            updates.set (0, 0.1f);
            updates.set (1, 0.2f);
            expect (updates.commit());

            updates.set (0, 0.3f);
            updates.set (1, 0.4f);
            expect (! updates.commit());
            expect (updates.getLatestState() == 1);

            expect (applyAll (updates, 1) == Updates { { 0, 0.1f }, { 1, 0.2f } });
            expect (updates.hasPendingUpdates());

            expect (updates.commit());
            expect (updates.getLatestState() == 2);
            expect (applyAll (updates, 2) == Updates { { 0, 0.3f }, { 1, 0.4f } });
        }

        beginTest ("The queue can be filled and emptied repeatedly");
        {
            DeferredParameterUpdates updates { 3, 5 };
            auto expected = 0.0f;

            for (int i = 0; i < 100; ++i)
            {
                for (size_t parameter = 0; parameter < updates.getNumParameters(); ++parameter)
                    updates.set (parameter, (float) i);

                expect (updates.commit());

                for (const auto& update : applyAll (updates, updates.getLatestState()))
                    expect (exactlyEqual (update.second, expected));

                expected += 1.0f;
            }

            expect (! updates.hasPendingUpdates());
        }

        beginTest ("A non-blocking caller gives up if updates are being applied elsewhere");
        {
            DeferredParameterUpdates updates { 2, 16 };

            updates.set (0, 0.1f);
            updates.set (1, 0.2f);
            expect (updates.commit());

            auto numAppliedElsewhere = 0;

            const auto update = updates.applyNextUpdate (1, Blocking::yes, [&] (size_t, float)
            {
                std::thread ([&]
                {
                    numAppliedElsewhere += (int) applyAll (updates, 1).size();
                }).join();
            });

            expect (update.has_value());
            expect (numAppliedElsewhere == 0);
            expect (applyAll (updates, 1) == Updates { { 1, 0.2f } });
        }

        beginTest ("Updates from another thread arrive in order");
        {
            constexpr size_t numParameters = 8;
            constexpr auto numStates = 20000;

            DeferredParameterUpdates updates { numParameters, 64 };
            std::atomic<int64_t> published { 0 };
            std::atomic<bool> finished { false };

            std::thread writer ([&]
            {
                Random random { 1234 };

                for (int state = 1; state <= numStates; ++state)
                {
                    for (size_t parameter = 0; parameter < numParameters; ++parameter)
                        if (parameter == 0 || state == numStates || random.nextBool())
                            updates.set (parameter, (float) state);

                    while (! updates.commit())
                        std::this_thread::yield();

                    published.store (updates.getLatestState());
                }

                finished.store (true);
            });

            std::array<float, numParameters> values {};
            auto inOrder = true;

            const auto consume = [&]
            {
                const auto state = published.load();

                while (const auto update = updates.applyNextUpdate (state, Blocking::no, [] (size_t, float) {}))
                {
                    auto& value = values[update->parameterIndex];
                    inOrder = inOrder && value < update->value && update->value <= (float) state;
                    value = update->value;
                }
            };

            while (! finished.load())
                consume();

            writer.join();
            consume();

            expect (inOrder);
            expect (! updates.hasPendingUpdates());

            for (const auto value : values)
                expect (exactlyEqual (value, (float) numStates));
        }

        beginTest ("Cancelling on another thread never lets a stale value through");
        {
            constexpr auto numIterations = 20000;

            DeferredParameterUpdates updates { 1, 64 };
            std::atomic<float> parameterValue { 0.0f };
            std::atomic<bool> finished { false };

            std::thread consumer ([&]
            {
                const auto setValue = [&] (size_t, float value) { parameterValue.store (value); };

                while (! finished.load())
                    while (updates.applyNextUpdate (std::numeric_limits<int64_t>::max(), Blocking::no, setValue).has_value()) {}
            });

            auto numStaleValues = 0;

            for (int i = 1; i <= numIterations; ++i)
            {
                updates.set (0, -1.0f);

                while (! updates.commit())
                    std::this_thread::yield();

                updates.cancel (0);
                parameterValue.store ((float) i);

                std::this_thread::yield();
                numStaleValues += exactlyEqual (parameterValue.load(), (float) i) ? 0 : 1;
            }

            finished.store (true);
            consumer.join();

            expect (numStaleValues == 0);
        }
    }
};

static DeferredParameterUpdatesTests deferredParameterUpdatesTests;

} // namespace juce
