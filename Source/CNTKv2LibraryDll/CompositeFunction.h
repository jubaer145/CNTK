//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#pragma once

#include "stdafx.h"
#include "CNTKLibrary.h"
#include "PrimitiveFunction.h"
#include "ComputationNetwork.h"
#include "BackCompat.h"
#include "Value.h"

namespace CNTK
{
    class CNTKBackPropState final : public BackPropState
    {
    public:
        CNTKBackPropState(const FunctionPtr& function, const DeviceDescriptor& computeDevice, const std::unordered_map<Variable, uint64_t>& backpropRootsForwardTimeStamps)
            : BackPropState(function, computeDevice), m_backpropRootsForwardTimeStamps(backpropRootsForwardTimeStamps)
        {}

        const std::unordered_map<Variable, uint64_t>& BackpropRootsForwardTimeStamps() const
        {
            return m_backpropRootsForwardTimeStamps; 
        }

    private:
        std::unordered_map<Variable, uint64_t> m_backpropRootsForwardTimeStamps;
    };
    typedef std::shared_ptr<CNTKBackPropState> CNTKBackPropStatePtr;

    class CompositeFunction;
    typedef std::shared_ptr<CompositeFunction> CompositeFunctionPtr;

    ///
    /// Represents a symbolic computation with zero or more input arguments and one or more outputs.
    /// Opposed to primitive functions, a composite function is composed of other Function instances whose inputs and outputs are wired together.
    /// CompositeFunction is also responsible for breaking the loop in case of cyclic graphs - it stores the pointers for to the child primitive
    /// functions and controls their lifetime.
    /// CompositeFunction class inherits thus from Function.
    ///
    class CompositeFunction final : public Function
    {
        friend class Function;
        friend class Trainer;
        friend class CompositeMinibatchSource;
        friend class PackedValue;

        template <typename T, typename ...CtorArgTypes>
        friend inline std::shared_ptr<T> MakeSharedObject(CtorArgTypes&& ...ctorArgs);

        friend void Internal::SaveAsLegacyModel(const FunctionPtr& rootFunction, const std::wstring& modelFile);

        friend void ComputeInputPerDimMeansAndInvStdDevs(const MinibatchSourcePtr& minibatchSource,
                                                         std::unordered_map<StreamInformation, std::pair<NDArrayViewPtr, NDArrayViewPtr>>& computedMeanAndInvStdDevs,
                                                         const DeviceDescriptor& device /*= DeviceDescriptor::CPUDevice()*/);

        static std::atomic<unsigned int> s_nextAutoGeneratedDynamicAxis;

        static const std::wstring CompositeFunctionOpName;

    public:
        static Axis NextAutoGeneratedDynamicAxis()
        {
            static const std::wstring s_autoGeneratedDynamicAxisNamePrefix = L"autoGeneratedDynamicAxis_";
            return Axis(s_autoGeneratedDynamicAxisNamePrefix + std::to_wstring(s_nextAutoGeneratedDynamicAxis++));
        }

    public:
        static CompositeFunctionPtr Create(const FunctionPtr& rootFunction, const std::wstring& name = L"", const std::wstring& uid = Internal::GenerateUid(L"CompositeFunction"))
        {
            std::unordered_set<FunctionPtr> visitedFunctions;

            // Call Collect to get the set of all functions in the graph
            Collect(rootFunction, visitedFunctions);

            auto composite = MakeSharedObject<CompositeFunction>(rootFunction, std::move(visitedFunctions), name, uid);

            // Initialize the outputs
            composite->InitOutputs();

            return composite;
        }

        BackPropStatePtr Forward(const std::unordered_map<Variable, ValuePtr>& arguments,
                                 std::unordered_map<Variable, ValuePtr>& outputs,
                                 const DeviceDescriptor& computeDevice,
                                 const std::unordered_set<Variable>& outputsToRetainBackwardStateFor,
                                 const std::unordered_set<Variable>& inputsToExcludeGradientsFor);

        virtual BackPropStatePtr Forward(const std::vector<ValuePtr>& /*inputValues*/,
                                         std::unordered_map<Variable, ValuePtr>& /*outputs*/,
                                         const DeviceDescriptor& /*computeDevice*/,
                                         const std::unordered_set<Variable>& /*outputsToRetainBackwardStateFor*/) override
        {
            NOT_IMPLEMENTED;
        }

        void InferOutputs(std::vector<Variable>& outputs) override
        {
            auto& inferred = m_rootFunction->InitOutputs();
            outputs.assign(inferred.begin(), inferred.end());
        }

        virtual void Backward(const BackPropStatePtr& state,
                              const std::unordered_map<Variable, ValuePtr>& rootGradientValues,
                              std::unordered_map<Variable, ValuePtr>& backPropagatedGradientValuesForInputs) override;

        Dictionary SerializeBlockComposite() const;

        virtual Dictionary Serialize() const override;

        virtual size_t CurrentVersion() const override { return s_serializationVersion; }

        static FunctionPtr DeserializeBlockComposite(const Dictionary& dict,
                                                     const std::unordered_set<FunctionPtr>& allPrimitiveFunctions,
                                                     const std::unordered_map<Variable, Variable>& allPlaceholderReplacements,
                                                     const CNTK::DeviceDescriptor& device);

        static FunctionPtr Deserialize(const Dictionary& dictionary, const CNTK::DeviceDescriptor& device, const UDFDeserializerPtr& deserializer);

        virtual const std::wstring& OpName() const override
        {
            return CompositeFunctionOpName;
        }

        template <typename FunctionType>
        static void PreorderTraverseVariables(const FunctionPtr& rootFunction, const FunctionType& functor, bool pythonOperandOrder = false)
        {
            TraverseVariables(rootFunction, functor, pythonOperandOrder, /*preOrder =*/ true);
        }

        template <typename FunctionType>
        static void PostorderTraverseVariables(const FunctionPtr& rootFunction, const FunctionType& functor, bool pythonOperandOrder = false)
        {
            TraverseVariables(rootFunction, functor, pythonOperandOrder, /*preOrder =*/ false);
        }

        template <typename FunctionType>
        static void TraverseVariables(const FunctionPtr& rootFunction, const FunctionType& functor, bool pythonOperandOrder, bool preOrder)
        {
            std::unordered_set<FunctionPtr> visitedFunctions;
            TraverseVariables(rootFunction, visitedFunctions, functor, pythonOperandOrder, preOrder);
        }

        // Recursively traverses the Function graph underlying the 'rootFunction' invoking the provided functor for all visited nodes in the graph.
        template <typename FunctionType>
        static void TraverseVariables(const FunctionPtr& rootFunction, std::unordered_set<FunctionPtr>& visitedFunctions, const FunctionType& functor, bool pythonOperandOrder, bool preOrder)
        {
            visitedFunctions.insert(rootFunction);
            auto rootFunctionOutputs = rootFunction->InitOutputs();

            if (preOrder)
            {
                for (const auto& rootOutput : rootFunctionOutputs)
                    functor(rootOutput);
            }

            auto rootFunctionInputs = rootFunction->Inputs(pythonOperandOrder);
            for (const auto& rootInput : rootFunctionInputs)
            {
                if (rootInput.IsOutput())
                {
                    if (visitedFunctions.find(rootInput.Owner()) == visitedFunctions.end())
                    {
                        const auto& function = rootInput.Owner();
                        TraverseVariables(function, visitedFunctions, functor, pythonOperandOrder, preOrder);
                    }
                }
                else
                    functor(rootInput);
            }

            if (!preOrder)
            {
                for (const auto& rootOutput : rootFunctionOutputs)
                    functor(rootOutput);
            }
        }

    private:
        // Replace any PlaceHolder Variables in the graph of Functions underlying 'this' CompositeFunction. All PlaceHolder variables
        // should have been replaced before performing any Forward compute of 'this' Function.
        virtual void OnPlaceholdersReplaced(const std::unordered_map<Variable, Variable>& placeholderReplacements,
                                            std::unordered_set<Variable>& replacedPlaceholders) override
        {
            // If any of the placeholders were replaced with Output variables, let's add the graph of function underneath 
            // each of those to 'm_allPrimitiveFunctions' set
            for (auto replacedPlaceholder : replacedPlaceholders)
            {
                auto replacingVariable = placeholderReplacements.at(replacedPlaceholder);
                if (replacingVariable.IsOutput())
                {
                    auto ownerFunc = replacingVariable.Owner();
                    std::unordered_set<FunctionPtr> visitedFunctions2;
                    Collect(ownerFunc, visitedFunctions2);

                    // Add the newly visited functions to 'm_allPrimitiveFunctions' set
                    m_allPrimitiveFunctions.insert(visitedFunctions2.begin(), visitedFunctions2.end());
                }
            }
        }

        CompositeFunction(const FunctionPtr& rootFunction, std::unordered_set<FunctionPtr>&& allPrimitiveFunctions, const std::wstring& name, const std::wstring& uid = Internal::GenerateUid(L"CompositeFunction"))
            : Function({}, Dictionary(), rootFunction, name, uid),
            m_allPrimitiveFunctions(std::move(allPrimitiveFunctions)), m_networkMatricesAllocated(false)
        {}

        std::vector<Variable> DetermineInputs(bool pythonOperandOrder = false) const
        {
            const auto& root = RootFunction();
            std::unordered_set<FunctionPtr> visitedFunctions;
            return DetermineInputs(root, visitedFunctions, pythonOperandOrder);
        }

         // Recursively traverses the Function graph and populates the provided set of functions.
        static void Collect(const FunctionPtr& rootFunction, std::unordered_set<FunctionPtr>& functions)
        {
            // Call Traverse to get the set of all functions in the graph
            PreorderTraverseFunctions(rootFunction, functions, [](const FunctionPtr& f){});
        }

        // Recursively traverses the Function graph underlying the 'rootFunction' to determine all the leaves (aka inputs) of the graph
        static std::vector<Variable> DetermineInputs(const FunctionPtr& rootFunction, std::unordered_set<FunctionPtr>& visitedFunctions, bool pythonOperandOrder = false)
        {
            vector<FunctionPtr> functions;
            std::vector<Variable> inputs;
            std::unordered_set<Variable> uniqueInputs;
            TraverseVariables(rootFunction, visitedFunctions, [&inputs, &uniqueInputs](const Variable& var) {
                if (!var.IsOutput() && uniqueInputs.find(var) == uniqueInputs.end())
                {
                    inputs.push_back(var);
                    uniqueInputs.insert(var);
                }
           }, pythonOperandOrder, /*preOrder =*/ true);

            return inputs;
        }


        // Copy the internal state from the network into the function graph.
        void UpdateInternalState() const;

        // Generate a dictionary representing the internal (local) state of the function graph.
        Dictionary GetInternalState() const;

        // Update the internal state using the provided dictionary. 
        // If the network is already created, directly update its state. Otherwise, copy the state from the 
        // dictionary into the function graph.
        void SetInternalState(const Dictionary& state);

        // Copy state info from source function graph into 'this' function graph.
        // Both graphs must be equivalent.
        void CopyState(const CompositeFunction& source);

        // This function is only needed for backwards compatibility to support deserializing composite funcitions that
        // stored the internal state inside a dedicated value in the dictionary.
        static void RestoreStatefulFunctions(size_t version, const Dictionary& dict, std::unordered_set<FunctionPtr> PrimitiveFunctions);

        static Variable GetMappingForNoOpOutput(const Variable& variable, bool recursive = false);
        static Variable GetMappingVariable(const Variable& variable, bool recursive = false);

        template <typename ElementType>
        Microsoft::MSR::CNTK::ComputationNetworkPtr GetComputationNetwork(const DeviceDescriptor& device,
                                                                          const std::unordered_set<Variable>& backpropRoots,
                                                                          const std::unordered_set<Variable>& outputs,
                                                                          const std::unordered_set<Variable>& inputsToExcludeGradientsFor,
                                                                          bool allocateNetworkMatrices);

        template <typename ElementType>
        static Microsoft::MSR::CNTK::ComputationNodeBasePtr CreateComputationNode(const Variable& variable,
                                                                                  Function* function,
                                                                                  const std::vector<std::shared_ptr<Microsoft::MSR::CNTK::ComputationNode<ElementType>>>& inputNodes,
                                                                                  Microsoft::MSR::CNTK::ComputationNetworkPtr& network,
                                                                                  std::unordered_map<Variable, Microsoft::MSR::CNTK::ComputationNodeBasePtr>& variableToNodeMap);

        template <typename ElementType>
        static Microsoft::MSR::CNTK::ComputationNodeBasePtr GetOutputVariableNode(const Variable& variable,
                                                                                  Microsoft::MSR::CNTK::ComputationNetworkPtr& network,
                                                                                  Microsoft::MSR::CNTK::ComputationNetworkBuilder<ElementType>& builder,
                                                                                  std::unordered_map<Variable, Microsoft::MSR::CNTK::ComputationNodeBasePtr>& variableToNodeMap,
                                                                                  std::unordered_map<Variable, bool>& isVariableRootMap,
                                                                                  const std::unordered_set<Variable>& inputsToExcludeGradientsFor);

        template <typename ElementType>
        static Microsoft::MSR::CNTK::ComputationNodeBasePtr GetNode(const Variable& variable, Microsoft::MSR::CNTK::ComputationNetworkPtr& network,
                                                                    Microsoft::MSR::CNTK::ComputationNetworkBuilder<ElementType>& builder,
                                                                    std::unordered_map<Variable, Microsoft::MSR::CNTK::ComputationNodeBasePtr>& variableToNodeMap,
                                                                    std::unordered_map<Variable, bool>& isVariableRootMap,
                                                                    const std::unordered_set<Variable>& inputsToExcludeGradientsFor);

        template <typename ElementType>
        static void PopulateComputationNodeValue(const std::pair<Variable, ValuePtr>& variableValue, Microsoft::MSR::CNTK::ComputationNodeBasePtr& computationNode, std::unordered_map< Microsoft::MSR::CNTK::MBLayoutPtr, Variable>& layoutsPopulated);
        void PopulateNetworkInputs(const std::unordered_map<Variable, ValuePtr>& arguments);

        template <typename ElementType>
        static void PopulateComputationNodeGradient(const std::pair<Variable, ValuePtr>& variableGradient, Microsoft::MSR::CNTK::ComputationNodeBasePtr& computationNode);
        void PopulateNetworkGradients(const std::unordered_map<Variable, ValuePtr>& gradients);

        static void GetNodeOutputOrGradient(Variable var, ValuePtr& varValue, Microsoft::MSR::CNTK::ComputationNodeBasePtr& computationNode, bool getGradient);
        void GetNetworkOutputs(std::unordered_map<Variable, ValuePtr>& outputs);
        void GetNetworkGradients(std::unordered_map<Variable, ValuePtr>& gradients);

        // Remove cyclic references for composite nodes
        static std::unordered_set<Variable> NonOwnerPreservingCopy(const std::unordered_set<Variable>& outputs);

        const std::vector<Variable>& GetArgumentDependencies(const Variable& output);

        std::unordered_map<Variable, uint64_t> GetCurrentBackpropRootsTimeStamps() const;

        void ClearExistingOutputOrGradientStorageReferences()
        {
            for (auto& existingStorageWeakReference : m_existingNetworkStorageReferences)
            {
                auto existingStorageReference = existingStorageWeakReference.lock();
                if (existingStorageReference)
                    existingStorageReference->Erase();
            }

            m_existingNetworkStorageReferences.clear();
        }

    private:

        // Set of all primitive functions in the graph underlying 'this' Function. Also keeps the primitive Function objects alive 
        // by holding strong references to them
        std::unordered_set<FunctionPtr> m_allPrimitiveFunctions;

        // A map from Variable objects to ComputationNode objects in the ComputationNetwork instance that implements 'this' Composite Function
        std::unordered_map<Variable, Microsoft::MSR::CNTK::ComputationNodeBasePtr> m_variableToNodeMap;

        // A map that tells whether a Variable in the graph underlying 'this' Function is a root of the graph
        std::unordered_map<Variable, bool> m_isVariableRootMap;

        Microsoft::MSR::CNTK::ComputationNetworkPtr m_computationNetwork;

        // Map to keep track of any references to network output/gradient storage handed out so far
        std::vector<PackedValueWeakPtr> m_existingNetworkStorageReferences;

        // The backpropRoots sepecified in the most recent 'Forward' call on 'this' Function.
        // This indicates for which of its roots has 'this' Function retained required intermediate 
        // states from the previos Forward call to be able to backpropagate gradients backwards from in
        // the next 'Backward' call.
        std::unordered_set<Variable> m_currentBackpropRoots;

        std::unordered_map<Variable, std::vector<Variable>> m_perOutputVarArgumentDependencies;

        bool m_networkMatricesAllocated;

        std::unordered_set<Variable> m_allNetworkRoots;

        std::unordered_map<Parameter, size_t> m_lastRecordedParameterValueTimeStamps;

        std::unordered_set<Variable> m_inputsExcludedFromGradientComputation;

        // Version history:
        // 1 -- initial version.
        // 2 -- add support for stateful functions (with corresponding nodes inheriting from RngUser).
        // 3 -- store internal function state directly in the attributes dictionary.
        static const size_t s_serializationVersion = 3;
    };
}
