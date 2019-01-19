type hook('a) = ..;

module HeterogenousList =
  HeterogenousList.Make({
    type t('a) = hook('a);
  });

type state('a, 'b) = option(HeterogenousList.t('a, 'b));

type t('a, 'b, 'c, 'd) = {
  remaining: option(HeterogenousList.t('a, 'b)),
  processed: HeterogenousList.t('c, 'd),
  onStateDidChange: unit => unit,
};

let createState = () => None;

let ofState = (remaining, ~onStateDidChange) => {
  remaining,
  processed: HeterogenousList.[],
  onStateDidChange,
};

let toState = ({processed}) => Some(processed);

let processNext =
    (
      ~default,
      ~merge=?,
      ~toWitness,
      {remaining, processed, onStateDidChange},
    ) =>
  switch (remaining) {
  | Some(l) =>
    let ({HeterogenousList.value, toWitness}, rest) =
      HeterogenousList.dropFirst(l);
    (
      value,
      {
        remaining: Some(rest),
        processed:
          HeterogenousList.append(
            processed,
            switch (merge) {
            | Some(f) => f(value)
            | None => value
            },
            toWitness,
          ),
        onStateDidChange,
      },
    );
  | None => (
      default,
      {
        remaining: None,
        processed: HeterogenousList.append(processed, default, toWitness),
        onStateDidChange,
      },
    )
  };

module State = {
  type t('a) = {
    currentValue: 'a,
    mutable nextValue: 'a,
  };

  type hook('a) +=
    | State(t('a)): hook(t('a));

  let make: 'a => t('a) =
    initialValue => {currentValue: initialValue, nextValue: initialValue};

  let wrapAsHook = s => State(s);

  let setState = (nextValue, stateContainer) => {
    stateContainer.nextValue = nextValue;
  };

  let flush = ({currentValue, nextValue}) =>
    if (currentValue === nextValue) {
      None;
    } else {
      Some({currentValue, nextValue: currentValue});
    };

  let hook = (initialState, hooks) => {
    let (stateContainer, nextHooks) =
      processNext(~default=make(initialState), ~toWitness=wrapAsHook, hooks);

    let setter = nextState => {
      setState(nextState, stateContainer);
      hooks.onStateDidChange();
    };

    (stateContainer.currentValue, setter, nextHooks);
  };
};

module Reducer = {
  type t('a) = {
    currentValue: 'a,
    mutable updates: list('a => 'a),
  };

  type hook('a) +=
    | Reducer(t('a)): hook(t('a));

  let make: 'a => t('a) =
    initialValue => {currentValue: initialValue, updates: []};

  let flush: t('a) => option(t('a)) =
    reducerState => {
      let {currentValue, updates} = reducerState;
      let nextValue =
        List.fold_right(
          (update, latestValue) => update(latestValue),
          updates,
          currentValue,
        );
      if (currentValue === nextValue) {
        reducerState.updates = [];
        None;
      } else {
        Some({currentValue: nextValue, updates: []});
      };
    };

  let wrapAsHook = s => Reducer(s);

  let enqueueUpdate = (nextUpdate, {updates} as stateContainer) => {
    stateContainer.updates = [nextUpdate, ...updates];
  };

  let hook = (~initialState, reducer, hooks) => {
    let (stateContainer, hooks) =
      processNext(~default=make(initialState), ~toWitness=wrapAsHook, hooks);

    let dispatch = action => {
      enqueueUpdate(prevValue => reducer(action, prevValue), stateContainer);
      hooks.onStateDidChange();
    };

    (stateContainer.currentValue, dispatch, hooks);
  };
};

module Ref = {
  type t('a) = ref('a);
  type hook('a) +=
    | Ref(t('a)): hook(t('a));
  let wrapAsHook = s => Ref(s);

  let hook = (initialState, hooks) => {
    let (internalRef, hooks) =
      processNext(~default=ref(initialState), ~toWitness=wrapAsHook, hooks);

    let setter = nextValue => internalRef := nextValue;

    (internalRef^, setter, hooks);
  };
};

module Effect = {
  type lifecycle =
    | Mount
    | Unmount
    | Update;
  type always;
  type onMount;
  type condition('a) =
    | Always: condition(always)
    | OnMount: condition(onMount)
    | If(('a, 'a) => bool, 'a): condition('a);
  type handler = unit => option(unit => unit);
  type t('a) = {
    condition: condition('a),
    handler: unit => option(unit => unit),
    mutable cleanupHandler: option(unit => unit),
    mutable previousCondition: condition('a),
  };
  type hook('a) +=
    | Effect(t('a)): hook(t('a));

  let wrapAsHook = s => Effect(s);

  let executeOptionalHandler =
    fun
    | Some(f) => {
        f();
        true;
      }
    | None => false;

  let executeIfNeeded:
    type conditionValue. (~lifecycle: lifecycle, t(conditionValue)) => bool =
    (~lifecycle, state) => {
      let {condition, previousCondition, handler, cleanupHandler} = state;
      switch (previousCondition) {
      | Always =>
        ignore(executeOptionalHandler(cleanupHandler));
        state.cleanupHandler = handler();
        true;
      | If(comparator, previousConditionValue) =>
        switch (lifecycle) {
        | Mount
        | Update =>
          let currentConditionValue =
            switch (condition) {
            | If(_, currentConditionValue) => currentConditionValue
            /* The following cases are unreachable because it's
             * Impossible to create a value of type condition(always)
             * Or condition(onMount) using the If constructor
             */
            | Always => previousConditionValue
            | OnMount => previousConditionValue
            };
          if (comparator(previousConditionValue, currentConditionValue)) {
            ignore(executeOptionalHandler(cleanupHandler));
            state.previousCondition = condition;
            state.cleanupHandler = handler();
            true;
          } else {
            state.previousCondition = condition;
            false;
          };
        | Unmount => executeOptionalHandler(cleanupHandler)
        }
      | OnMount =>
        switch (lifecycle) {
        | Mount =>
          state.cleanupHandler = handler();
          true;
        | Unmount => executeOptionalHandler(cleanupHandler)
        | _ => false
        }
      };
    };

  let hook = (condition, handler, hooks) => {
    let (_, hooks) =
      processNext(
        ~default={
          condition,
          handler,
          cleanupHandler: None,
          previousCondition: condition,
        },
        ~merge=prevEffect => {...prevEffect, condition, handler},
        ~toWitness=wrapAsHook,
        hooks,
      );
    hooks;
  };
};

let state = State.hook;
let reducer = Reducer.hook;
let ref = Ref.hook;
let effect = Effect.hook;

let executeEffects = (~lifecycle, hooks) => {
  switch (hooks) {
  | None => false
  | Some(hooks) =>
    HeterogenousList.fold(
      (shouldUpdate, opaqueValue) =>
        switch (opaqueValue) {
        | HeterogenousList.Any(Effect.Effect(state)) =>
          Effect.executeIfNeeded(~lifecycle, state) || shouldUpdate
        | _ => shouldUpdate
        },
      false,
      hooks,
    )
  };
};

let flushPendingStateUpdates = hooks =>
  switch (hooks) {
  | Some(hooks) =>
    HeterogenousList.map(
      {
        f: (type a, hook: hook(a)) => {
          switch (hook) {
          | Reducer.Reducer(s) => (Reducer.flush(s): option(a))
          | State.State(s) => (State.flush(s): option(a))
          | _ => None
          };
        },
      },
      hooks,
    )
    |> HeterogenousList.compareElementsIdentity(hooks) == false
  | None => false
  };
