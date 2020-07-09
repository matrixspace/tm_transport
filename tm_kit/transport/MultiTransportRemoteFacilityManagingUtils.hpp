#ifndef TM_KIT_TRANSPORT_MULTI_TRANSPORT_REMOTE_FACILITY_MANAGING_UTILS_HPP_
#define TM_KIT_TRANSPORT_MULTI_TRANSPORT_REMOTE_FACILITY_MANAGING_UTILS_HPP_

#include <tm_kit/transport/MultiTransportRemoteFacility.hpp>
#include <tm_kit/transport/MultiTransportBroadcastListener.hpp>
#include <tm_kit/transport/HeartbeatMessageToRemoteFacilityCommand.hpp>

#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockImporter.hpp>
#include <tm_kit/basic/CommonFlowUtils.hpp>
#include <tm_kit/basic/MonadRunnerUtils.hpp>

namespace dev { namespace cd606 { namespace tm { namespace transport {

    template <class R>
    class MultiTransportRemoteFacilityManagingUtils {
    public:
        using M = typename R::MonadType;

        template <class ... IdentitiesAndInputsAndOutputs>
        using NonDistinguishedRemoteFacilities =
            std::tuple<
                typename R::template FacilitioidConnector<
                    std::tuple_element_t<1,IdentitiesAndInputsAndOutputs>
                    , std::tuple_element_t<2,IdentitiesAndInputsAndOutputs>
                >...
            >;

    private:
        template <
            int CurrentIdx
            , class Output
        >
        static void setupNonDistinguishedRemoteFacilitiesInternal(
            R &r
            , typename R::template Source<
                std::array<MultiTransportRemoteFacilityAction, CurrentIdx>
            > &&actionSource
            , std::array<std::string, CurrentIdx> const &names
            , std::string const &prefix
            , std::function<std::optional<ByteDataHookPair>(std::string const &, ConnectionLocator const &)> const &hookPairFactory
            , Output &output
        ) 
        {}

        template <
            int CurrentIdx
            , class Output
            , class FirstRemainingIdentityAndInputAndOutput
            , class ... RemainingIdentitiesAndInputsAndOutputs
        >
        static void setupNonDistinguishedRemoteFacilitiesInternal(
            R &r
            , typename R::template Source<
                std::array<MultiTransportRemoteFacilityAction, sizeof...(RemainingIdentitiesAndInputsAndOutputs)+CurrentIdx+1>
            > &&actionSource
            , std::array<std::string, sizeof...(RemainingIdentitiesAndInputsAndOutputs)+CurrentIdx+1> const &names
            , std::string const &prefix
            , std::function<std::optional<ByteDataHookPair>(std::string const &, ConnectionLocator const &)> const &hookPairFactory
            , Output &output
        ) {
            auto facility = M::localOnOrderFacility(
                new MultiTransportRemoteFacility<
                    typename M::EnvironmentType
                    , std::tuple_element_t<1, FirstRemainingIdentityAndInputAndOutput>
                    , std::tuple_element_t<2, FirstRemainingIdentityAndInputAndOutput>
                    , std::tuple_element_t<0, FirstRemainingIdentityAndInputAndOutput>
                    , transport::MultiTransportRemoteFacilityDispatchStrategy::Random
                >(hookPairFactory)
            );
            r.registerLocalOnOrderFacility(
                prefix+"/"+names[CurrentIdx]
                , facility
            );
            auto getOneAction = M::template liftPure<std::array<MultiTransportRemoteFacilityAction, sizeof...(RemainingIdentitiesAndInputsAndOutputs)+CurrentIdx+1>>(
                [](std::array<MultiTransportRemoteFacilityAction, sizeof...(RemainingIdentitiesAndInputsAndOutputs)+CurrentIdx+1> &&x) -> MultiTransportRemoteFacilityAction {
                    return x[CurrentIdx];
                }
            );
            r.registerAction(prefix+"/get_"+std::to_string(CurrentIdx), getOneAction);
            r.connect(
                r.execute(getOneAction, actionSource.clone())
                , r.localFacilityAsSink(facility)
            );
            std::get<CurrentIdx>(output) = R::localFacilityConnector(facility);
            setupNonDistinguishedRemoteFacilitiesInternal<
                (CurrentIdx+1)
                , Output
                , RemainingIdentitiesAndInputsAndOutputs...
            >(
                r 
                , actionSource.clone()
                , names
                , prefix
                , hookPairFactory
                , output
            );
        }
    public:
        template <class ... IdentitiesAndInputsAndOutputs>
        static auto setupNonDistinguishedRemoteFacilities(
            R &r
            , typename R::template Source<
                std::array<MultiTransportRemoteFacilityAction, sizeof...(IdentitiesAndInputsAndOutputs)>
            > &&actionSource
            , std::array<std::string, sizeof...(IdentitiesAndInputsAndOutputs)> const &names
            , std::string const &prefix
            , std::function<std::optional<ByteDataHookPair>(std::string const &, ConnectionLocator const &)> const &hookPairFactory
                = [](std::string const &, ConnectionLocator const &) {return std::nullopt;}
        ) -> NonDistinguishedRemoteFacilities<IdentitiesAndInputsAndOutputs...>
        {
            if constexpr (sizeof...(IdentitiesAndInputsAndOutputs) < 1) {
                return {};
            } else {
                using Output = NonDistinguishedRemoteFacilities<IdentitiesAndInputsAndOutputs...>;
                Output output;
                setupNonDistinguishedRemoteFacilitiesInternal<
                    0
                    , Output
                    , IdentitiesAndInputsAndOutputs...
                >(
                    r 
                    , std::move(actionSource)
                    , names
                    , prefix
                    , hookPairFactory
                    , output
                );
                return output;
            }
        }

    public:
        template <class Input, class Output>
        struct DistinguishedRemoteFacility {
            typename R::template Sinkoid<
                typename M::template Key<std::tuple<ConnectionLocator, Input>>
            > orderReceiver;
            typename R::template Sourceoid<
                typename M::template KeyedData<std::tuple<ConnectionLocator, Input>, Output> 
            > feedOrderResults;
            typename R::template Sourceoid<
                std::tuple<ConnectionLocator, bool>
            > feedConnectionChanges;
        };

        template <class ... IdentitiesAndInputsAndOutputs>
        using DistinguishedRemoteFacilities =
            std::tuple<
                DistinguishedRemoteFacility<
                    std::tuple_element_t<1,IdentitiesAndInputsAndOutputs>
                    , std::tuple_element_t<2,IdentitiesAndInputsAndOutputs>
                >...
            >;

        template <class ... IdentitiesAndInputsAndOutputs>
        using DistinguishedRemoteFacilityInitiators =
            std::tuple<
                std::function<
                    std::tuple_element_t<1,IdentitiesAndInputsAndOutputs>()
                >...
            >;

        template <class Input, class Output>
        using DistinguishedRemoteFacilityInitialCallbackPicker =
            std::function<bool(Input const &, Output const &)>;

        template <class ... IdentitiesAndInputsAndOutputs>
        using DistinguishedRemoteFacilityInitialCallbackPickers =
            std::tuple<
                DistinguishedRemoteFacilityInitialCallbackPicker<
                    std::tuple_element_t<1,IdentitiesAndInputsAndOutputs>
                    , std::tuple_element_t<2,IdentitiesAndInputsAndOutputs>
                >...
            >;

    private:
        template <
            int CurrentIdx
            , class Initiators
            , class InitialCallbackPickers
            , class Output
        >
        static void setupDistinguishedRemoteFacilitiesInternal(
            R &r
            , typename R::template Source<
                std::array<MultiTransportRemoteFacilityAction, CurrentIdx>
            > &&actionSource
            , Initiators const &initiators
            , InitialCallbackPickers const &initialCallbackPickers
            , std::array<std::string, CurrentIdx> const &names
            , std::string const &prefix
            , std::function<std::optional<ByteDataHookPair>(std::string const &, ConnectionLocator const &)> const &hookPairFactory
            , bool lastOneNeedsToBeTiedUp
            , Output &output
            , typename R::template Source<
                std::array<MultiTransportRemoteFacilityAction, CurrentIdx>
            > *nextSourceOutput
        ) 
        {
            if (nextSourceOutput) {
                *nextSourceOutput = std::move(actionSource);
            }
        }

        template <
            int CurrentIdx
            , class Initiators
            , class InitialCallbackPickers
            , class Output
            , class FirstRemainingIdentityAndInputAndOutput
            , class ... RemainingIdentitiesAndInputsAndOutputs
        >
        static void setupDistinguishedRemoteFacilitiesInternal(
            R &r
            , typename R::template Source<
                std::array<MultiTransportRemoteFacilityAction, sizeof...(RemainingIdentitiesAndInputsAndOutputs)+CurrentIdx+1>
            > &&actionSource
            , Initiators const &initiators
            , InitialCallbackPickers const &initialCallbackPickers
            , std::array<std::string, sizeof...(RemainingIdentitiesAndInputsAndOutputs)+CurrentIdx+1> const &names
            , std::string const &prefix
            , std::function<std::optional<ByteDataHookPair>(std::string const &, ConnectionLocator const &)> const &hookPairFactory
            , bool lastOneNeedsToBeTiedUp
            , Output &output
            , typename R::template Source<
                std::array<MultiTransportRemoteFacilityAction, sizeof...(RemainingIdentitiesAndInputsAndOutputs)+CurrentIdx+1>
            > *nextSourceOutput
        ) {
            using InputArray = std::array<MultiTransportRemoteFacilityAction, sizeof...(RemainingIdentitiesAndInputsAndOutputs)+CurrentIdx+1>;
            
            auto vieInputInject = [](InputArray &&x) ->  MultiTransportRemoteFacilityAction {
                return std::move(x[CurrentIdx]);
            };
            auto vieExtraOutputClassifier = 
                [](transport::MultiTransportRemoteFacilityActionResult const &res) -> bool {
                    return (std::get<0>(res).actionType == transport::MultiTransportRemoteFacilityActionType::Register);
                };
            
            auto initiator = std::get<CurrentIdx>(initiators);
            auto vieSelfLoopCreator = [initiator](MultiTransportRemoteFacilityActionResult &&x) 
                -> std::tuple<
                    ConnectionLocator
                    , std::tuple_element_t<1, FirstRemainingIdentityAndInputAndOutput>
                >
            {
                return {std::get<0>(x).connectionLocator, initiator()};
            };

            auto initialCallbackPicker = std::get<CurrentIdx>(initialCallbackPickers);

            auto vieInitialCallbackFilter =
                [initialCallbackPicker](typename M::template KeyedData<
                    std::tuple<
                        ConnectionLocator
                        , std::tuple_element_t<1, FirstRemainingIdentityAndInputAndOutput>
                    >
                    , std::tuple_element_t<2, FirstRemainingIdentityAndInputAndOutput>
                > const &x) -> bool {
                    return initialCallbackPicker(std::get<1>(x.key.key()), x.data);
                };

            auto facility = M::vieOnOrderFacility(
                new transport::MultiTransportRemoteFacility<
                    typename M::EnvironmentType
                    , std::tuple_element_t<1, FirstRemainingIdentityAndInputAndOutput>
                    , std::tuple_element_t<2, FirstRemainingIdentityAndInputAndOutput>
                    , std::tuple_element_t<0, FirstRemainingIdentityAndInputAndOutput>
                    , transport::MultiTransportRemoteFacilityDispatchStrategy::Designated
                >
            );

            auto facilityLoopOutput = basic::MonadRunnerUtilComponents<R>::setupVIEFacilitySelfLoopAndWait(
                r
                , actionSource.clone()
                , std::move(vieInputInject)
                , std::move(vieExtraOutputClassifier)
                , std::move(vieSelfLoopCreator)
                , facility
                , std::move(vieInitialCallbackFilter)
                , prefix+"/"+names[CurrentIdx]+"/loop"
                , names[CurrentIdx]
            );

            auto extraOutputConverter = M::template liftPure<transport::MultiTransportRemoteFacilityActionResult>(
                [](transport::MultiTransportRemoteFacilityActionResult &&x) 
                    -> std::tuple<ConnectionLocator, bool>
                {
                    return {
                        std::get<0>(x).connectionLocator
                        , (std::get<0>(x).actionType == MultiTransportRemoteFacilityActionType::Register)
                    };
                }
            );

            r.registerAction(prefix+"/"+names[CurrentIdx]+"/loop/extraOutputConverter", extraOutputConverter);

            auto extraOutput = r.execute(extraOutputConverter, facilityLoopOutput.facilityExtraOutput.clone());

            auto extraOutputCatcher = M::template trivialExporter<std::tuple<ConnectionLocator, bool>>();
            r.registerExporter(prefix+"/"+names[CurrentIdx]+"/loop/extraOutputCatcher", extraOutputCatcher);
            r.exportItem(extraOutputCatcher, extraOutput.clone());

            std::get<CurrentIdx>(output) = 
                DistinguishedRemoteFacility<
                    std::tuple_element_t<1, FirstRemainingIdentityAndInputAndOutput>
                    , std::tuple_element_t<2, FirstRemainingIdentityAndInputAndOutput>
                > {
                    facilityLoopOutput.callIntoFacility
                    , r.sourceAsSourceoid(facilityLoopOutput.facilityOutput.clone())
                    , r.sourceAsSourceoid(extraOutput.clone())
                };

            if constexpr (sizeof...(RemainingIdentitiesAndInputsAndOutputs) == 0) {
                if (lastOneNeedsToBeTiedUp) {
                    auto emptyExporter = M::template trivialExporter<InputArray>();
                    r.exportItem(prefix+"/"+names[CurrentIdx]+"/loop/emptyExporter", emptyExporter, facilityLoopOutput.nextTriggeringSource.clone());
                }
                if (nextSourceOutput) {
                    *nextSourceOutput = facilityLoopOutput.nextTriggeringSource;
                }
            } else {
                setupDistinguishedRemoteFacilitiesInternal<
                    (CurrentIdx+1)
                    , Initiators
                    , Output
                    , RemainingIdentitiesAndInputsAndOutputs...
                >(
                    r 
                    , std::move(facilityLoopOutput.nextTriggeringSource)
                    , initiators
                    , names
                    , prefix
                    , hookPairFactory
                    , lastOneNeedsToBeTiedUp
                    , output
                    , nextSourceOutput
                );
            }
        }
    public:
        template <class ... IdentitiesAndInputsAndOutputs>
        static auto setupDistinguishedRemoteFacilities(
            R &r
            , typename R::template Source<
                std::array<MultiTransportRemoteFacilityAction, sizeof...(IdentitiesAndInputsAndOutputs)>
            > &&actionSource
            , DistinguishedRemoteFacilityInitiators<IdentitiesAndInputsAndOutputs...> const &initiators
            , DistinguishedRemoteFacilityInitialCallbackPickers<IdentitiesAndInputsAndOutputs...> const &initialCallbackPickers
            , std::array<std::string, sizeof...(IdentitiesAndInputsAndOutputs)> const &names
            , std::string const &prefix
            , std::function<std::optional<ByteDataHookPair>(std::string const &, ConnectionLocator const &)> const &hookPairFactory
                = [](std::string const &, ConnectionLocator const &) {return std::nullopt;}
            , bool lastOneNeedsToBeTiedUp = true
            , typename R::template Source<
                std::array<MultiTransportRemoteFacilityAction, sizeof...(IdentitiesAndInputsAndOutputs)>
            > *nextTriggeringSource = nullptr
        ) -> DistinguishedRemoteFacilities<IdentitiesAndInputsAndOutputs...>
        {
            if constexpr (sizeof...(IdentitiesAndInputsAndOutputs) < 1) {
                return {};
            } else {
                using Initiators = DistinguishedRemoteFacilityInitiators<IdentitiesAndInputsAndOutputs...>;
                using InitialCallbackPickers = DistinguishedRemoteFacilityInitialCallbackPickers<IdentitiesAndInputsAndOutputs...>;
                using Output = DistinguishedRemoteFacilities<IdentitiesAndInputsAndOutputs...>;
                Output output;
                setupDistinguishedRemoteFacilitiesInternal<
                    0
                    , Initiators
                    , InitialCallbackPickers
                    , Output
                    , IdentitiesAndInputsAndOutputs...
                >(
                    r 
                    , std::move(actionSource)
                    , initiators
                    , initialCallbackPickers
                    , names
                    , prefix
                    , hookPairFactory
                    , lastOneNeedsToBeTiedUp
                    , output
                    , nextTriggeringSource
                );
                return output;
            }
        }
    
    public:
        template <class ... Specs>
        class SetupRemoteFacilities {};

        template <class ... DistinguishedRemoteFacilitiesSpec, class ... NonDistinguishedRemoteFacilitiesSpec>
        class SetupRemoteFacilities<
            std::tuple<DistinguishedRemoteFacilitiesSpec...>
            , std::tuple<NonDistinguishedRemoteFacilitiesSpec...>
        > {
        public:
            static auto run(
                R &r
                , MultiTransportBroadcastListenerAddSubscription const &heartbeatSpec
                , std::regex const &serverNameRE
                , std::array<std::string, sizeof...(DistinguishedRemoteFacilitiesSpec)+sizeof...(NonDistinguishedRemoteFacilitiesSpec)> const &channelNames
                , std::chrono::system_clock::duration ttl
                , std::chrono::system_clock::duration checkPeriod
                , DistinguishedRemoteFacilityInitiators<DistinguishedRemoteFacilitiesSpec...> const &initiators
                , DistinguishedRemoteFacilityInitialCallbackPickers<DistinguishedRemoteFacilitiesSpec...> const &initialCallbackPickers
                , std::array<std::string, sizeof...(DistinguishedRemoteFacilitiesSpec)+sizeof...(NonDistinguishedRemoteFacilitiesSpec)> const &facilityRegistrationNames
                , std::string const &prefix
                , std::optional<WireToUserHook> wireToUserHookForHeartbeat = std::nullopt
                , std::function<std::optional<ByteDataHookPair>(std::string const &, ConnectionLocator const &)> const &hookPairFactory
                    = [](std::string const &, ConnectionLocator const &) {return std::nullopt;}
            ) 
            -> std::tuple<
                DistinguishedRemoteFacilities<DistinguishedRemoteFacilitiesSpec...>
                , NonDistinguishedRemoteFacilities<NonDistinguishedRemoteFacilitiesSpec...>
            >
            {
                std::array<std::string, sizeof...(DistinguishedRemoteFacilitiesSpec)> distinguishedChannelNames;
                std::array<std::string, sizeof...(NonDistinguishedRemoteFacilitiesSpec)> nonDistinguishedChannelNames;
                for (int ii=0; ii<sizeof...(DistinguishedRemoteFacilitiesSpec); ++ii) {
                    distinguishedChannelNames[ii] = channelNames[ii];
                }
                for (int ii=0; ii<sizeof...(NonDistinguishedRemoteFacilitiesSpec); ++ii) {
                    nonDistinguishedChannelNames[ii] = channelNames[ii+sizeof...(DistinguishedRemoteFacilitiesSpec)];
                }
                std::array<std::string, sizeof...(DistinguishedRemoteFacilitiesSpec)> distinguishedRegNames;
                std::array<std::string, sizeof...(NonDistinguishedRemoteFacilitiesSpec)> nonDistinguishedRegNames;
                for (int ii=0; ii<sizeof...(DistinguishedRemoteFacilitiesSpec); ++ii) {
                    distinguishedRegNames[ii] = facilityRegistrationNames[ii];
                }
                for (int ii=0; ii<sizeof...(NonDistinguishedRemoteFacilitiesSpec); ++ii) {
                    nonDistinguishedRegNames[ii] = facilityRegistrationNames[ii+sizeof...(DistinguishedRemoteFacilitiesSpec)];
                }

                auto createHeartbeatListenKey = M::constFirstPushKeyImporter(
                    transport::MultiTransportBroadcastListenerInput { {
                        heartbeatSpec
                    } }
                );
                r.registerImporter(prefix+"/createHeartbeatListenKey", createHeartbeatListenKey);

                auto discardTopicFromHeartbeat = M::template liftPure<basic::TypedDataWithTopic<HeartbeatMessage>>(
                    [](basic::TypedDataWithTopic<HeartbeatMessage> &&d) {
                        return std::move(d.content);
                    }
                );
                r.registerAction(prefix+"/discardTopicFromHeartbeat", discardTopicFromHeartbeat);

                auto heartbeatSource = basic::MonadRunnerUtilComponents<R>::importWithTrigger(
                    r
                    , r.importItem(createHeartbeatListenKey)
                    , M::onOrderFacilityWithExternalEffects(
                        new transport::MultiTransportBroadcastListener<typename M::EnvironmentType, HeartbeatMessage>(wireToUserHookForHeartbeat)
                    )
                    , std::nullopt //the trigger response is not needed
                    , prefix+"/heartbeatListener"
                );

                auto facilityCheckTimer = basic::real_time_clock::ClockImporter<typename M::EnvironmentType>
                    ::template createRecurringClockImporter<basic::VoidStruct>(
                        std::chrono::system_clock::now()
                        , std::chrono::system_clock::now()+std::chrono::hours(24)
                        , checkPeriod
                        , [](typename M::EnvironmentType::TimePointType const &tp) {
                            return basic::VoidStruct {};
                        }
                    );
                auto timerInput = r.importItem(prefix+"/checkTimer", facilityCheckTimer);

                auto heartbeatParser = M::template enhancedMulti2<
                    HeartbeatMessage, basic::VoidStruct
                >(
                    HeartbeatMessageToRemoteFacilityCommandConverter<
                        std::tuple<
                            DistinguishedRemoteFacilitiesSpec...
                        >
                        , std::tuple<
                            NonDistinguishedRemoteFacilitiesSpec...
                        >
                    >(
                        serverNameRE
                        , distinguishedChannelNames
                        , nonDistinguishedChannelNames
                        , ttl
                    )
                );
                r.registerAction(prefix+"/heartbeatParser", heartbeatParser);

                auto distinguishedSpecInputBrancher = M::template liftPure<
                    std::tuple<
                        std::array<MultiTransportRemoteFacilityAction, sizeof...(DistinguishedRemoteFacilitiesSpec)>
                        , std::array<MultiTransportRemoteFacilityAction, sizeof...(NonDistinguishedRemoteFacilitiesSpec)>
                    >
                >(
                    [](
                        std::tuple<
                            std::array<MultiTransportRemoteFacilityAction, sizeof...(DistinguishedRemoteFacilitiesSpec)>
                            , std::array<MultiTransportRemoteFacilityAction, sizeof...(NonDistinguishedRemoteFacilitiesSpec)>
                        > &&x
                    ) -> std::array<MultiTransportRemoteFacilityAction, sizeof...(DistinguishedRemoteFacilitiesSpec)> {
                        return std::get<0>(x);
                    }
                );
                auto nonDistinguishedSpecInputBrancher = M::template liftPure<
                    std::tuple<
                        std::array<MultiTransportRemoteFacilityAction, sizeof...(DistinguishedRemoteFacilitiesSpec)>
                        , std::array<MultiTransportRemoteFacilityAction, sizeof...(NonDistinguishedRemoteFacilitiesSpec)>
                    >
                >(
                    [](
                        std::tuple<
                            std::array<MultiTransportRemoteFacilityAction, sizeof...(DistinguishedRemoteFacilitiesSpec)>
                            , std::array<MultiTransportRemoteFacilityAction, sizeof...(NonDistinguishedRemoteFacilitiesSpec)>
                        > &&x
                    ) -> std::array<MultiTransportRemoteFacilityAction, sizeof...(NonDistinguishedRemoteFacilitiesSpec)> {
                        return std::get<1>(x);
                    }
                );

                r.registerAction(prefix+"/distinguishedBranch", distinguishedSpecInputBrancher);
                r.registerAction(prefix+"/nonDistinguishedBranch", nonDistinguishedSpecInputBrancher);

                auto commands 
                    = r.execute(
                        heartbeatParser
                        , r.execute(
                            discardTopicFromHeartbeat
                            , std::move(heartbeatSource)
                        )
                    );
                r.execute(heartbeatParser, std::move(timerInput));
                auto distinguishedCommands
                    = r.execute(
                        distinguishedSpecInputBrancher
                        , commands.clone()
                    );
                auto nonDistinguishedCommands
                    = r.execute(
                        nonDistinguishedSpecInputBrancher
                        , commands.clone()
                    );

                auto nextTriggeringSource = distinguishedCommands.clone();
                auto distinguishedFacilities = 
                    setupDistinguishedRemoteFacilities<DistinguishedRemoteFacilitiesSpec...>
                    (
                        r 
                        , distinguishedCommands.clone()
                        , initiators
                        , initialCallbackPickers
                        , distinguishedRegNames
                        , prefix+"/facilities"
                        , hookPairFactory
                        , (sizeof...(NonDistinguishedRemoteFacilitiesSpec) == 0)
                        , &nextTriggeringSource
                    );

                if constexpr (sizeof...(NonDistinguishedRemoteFacilitiesSpec) > 0) {
                    auto synchronizerInput = M::template liftPure<
                        std::array<MultiTransportRemoteFacilityAction, sizeof...(DistinguishedRemoteFacilitiesSpec)>
                    >(
                        [](std::array<MultiTransportRemoteFacilityAction, sizeof...(DistinguishedRemoteFacilitiesSpec)> &&) 
                            -> basic::VoidStruct 
                        {
                            return basic::VoidStruct {};
                        }
                    );
                    r.registerAction(prefix+"/synchronizerInput", synchronizerInput);

                    auto synchronizer = basic::CommonFlowUtilComponents<M>::template synchronizer2<
                        basic::VoidStruct
                        , std::array<MultiTransportRemoteFacilityAction, sizeof...(NonDistinguishedRemoteFacilitiesSpec)>
                    >
                    (
                        [](
                            basic::VoidStruct
                            , std::array<MultiTransportRemoteFacilityAction, sizeof...(NonDistinguishedRemoteFacilitiesSpec)> &&x
                        ) -> std::array<MultiTransportRemoteFacilityAction, sizeof...(NonDistinguishedRemoteFacilitiesSpec)>
                        {
                            return std::move(x);
                        }
                    );
                    r.registerAction(prefix+"/synchronizer", synchronizer);

                    auto synchronizedNonDistinguishedCommands =
                        r.execute(
                            synchronizer
                            , r.execute(
                                synchronizerInput
                                , nextTriggeringSource.clone()
                            )
                        );
                    r.execute(synchronizer, nonDistinguishedCommands.clone());

                    auto nonDistinguishedFacilities = 
                        setupNonDistinguishedRemoteFacilities<NonDistinguishedRemoteFacilitiesSpec...>
                        (
                            r 
                            , std::move(synchronizedNonDistinguishedCommands)
                            , nonDistinguishedRegNames
                            , prefix+"/facilities"
                        );

                    return {
                        std::move(distinguishedFacilities)
                        , std::move(nonDistinguishedFacilities)
                    };
                } else {
                    return {
                        std::move(distinguishedFacilities)
                        , {}
                    };
                }               
            }

        };
    };
    
} } } }

#endif