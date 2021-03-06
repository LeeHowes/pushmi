#pragma once
// Copyright (c) 2018-present, Facebook, Inc.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "single.h"

namespace pushmi {

template <class V, class E = std::exception_ptr>
class any_single_deferred {
  union data {
    void* pobj_ = nullptr;
    char buffer_[sizeof(V)]; // can hold a V in-situ
  } data_{};
  template <class Wrapped>
  static constexpr bool insitu() {
    return sizeof(Wrapped) <= sizeof(data::buffer_) &&
        std::is_nothrow_move_constructible<Wrapped>::value;
  }
  struct vtable {
    static void s_op(data&, data*) {}
    static void s_submit(data&, single<V, E>) {}
    void (*op_)(data&, data*) = vtable::s_op;
    void (*submit_)(data&, single<V, E>) = vtable::s_submit;
  };
  static constexpr vtable const noop_ {};
  vtable const* vptr_ = &noop_;
  template <class Wrapped>
  any_single_deferred(Wrapped obj, std::false_type) : any_single_deferred() {
    struct s {
      static void op(data& src, data* dst) {
        if (dst)
          dst->pobj_ = std::exchange(src.pobj_, nullptr);
        delete static_cast<Wrapped const*>(src.pobj_);
      }
      static void submit(data& src, single<V, E> out) {
        ::pushmi::submit(*static_cast<Wrapped*>(src.pobj_), std::move(out));
      }
    };
    static const vtable vtbl{s::op, s::submit};
    data_.pobj_ = new Wrapped(std::move(obj));
    vptr_ = &vtbl;
  }
  template <class Wrapped>
  any_single_deferred(Wrapped obj, std::true_type) noexcept
      : any_single_deferred() {
    struct s {
      static void op(data& src, data* dst) {
        if (dst)
          new (dst->buffer_) Wrapped(
              std::move(*static_cast<Wrapped*>((void*)src.buffer_)));
        static_cast<Wrapped const*>((void*)src.buffer_)->~Wrapped();
      }
      static void submit(data& src, single<V, E> out) {
        ::pushmi::submit(
            *static_cast<Wrapped*>((void*)src.buffer_), std::move(out));
      }
    };
    static const vtable vtbl{s::op, s::submit};
    new (data_.buffer_) Wrapped(std::move(obj));
    vptr_ = &vtbl;
  }
  template <class T, class U = std::decay_t<T>>
  using wrapped_t =
    std::enable_if_t<!std::is_same<U, any_single_deferred>::value, U>;
 public:
  using properties = property_set<is_sender<>, is_single<>>;

  any_single_deferred() = default;
  any_single_deferred(any_single_deferred&& that) noexcept
      : any_single_deferred() {
    that.vptr_->op_(that.data_, &data_);
    std::swap(that.vptr_, vptr_);
  }

  PUSHMI_TEMPLATE(class Wrapped)
    (requires SenderTo<wrapped_t<Wrapped>, single<V, E>, is_single<>>)
  explicit any_single_deferred(Wrapped obj) noexcept(insitu<Wrapped>())
    : any_single_deferred{std::move(obj), bool_<insitu<Wrapped>()>{}} {}
  ~any_single_deferred() {
    vptr_->op_(data_, nullptr);
  }
  any_single_deferred& operator=(any_single_deferred&& that) noexcept {
    this->~any_single_deferred();
    new ((void*)this) any_single_deferred(std::move(that));
    return *this;
  }
  void submit(single<V, E> out) {
    vptr_->submit_(data_, std::move(out));
  }
};

// Class static definitions:
template <class V, class E>
constexpr typename any_single_deferred<V, E>::vtable const
  any_single_deferred<V, E>::noop_;

template <class SF>
class single_deferred<SF> {
  SF sf_;

 public:
  using properties = property_set<is_sender<>, is_single<>>;

  constexpr single_deferred() = default;
  constexpr explicit single_deferred(SF sf)
      : sf_(std::move(sf)) {}

  PUSHMI_TEMPLATE(class Out)
    (requires PUSHMI_EXP(defer::Receiver<Out, is_single<>> PUSHMI_AND defer::Invocable<SF&, Out>))
  void submit(Out out) {
    sf_(std::move(out));
  }
};

namespace detail {
template <PUSHMI_TYPE_CONSTRAINT(Sender<is_single<>>) Data, class DSF>
class single_deferred_2 {
  Data data_;
  DSF sf_;

 public:
  using properties = property_set<is_sender<>, is_single<>>;

  constexpr single_deferred_2() = default;
  constexpr explicit single_deferred_2(Data data)
      : data_(std::move(data)) {}
  constexpr single_deferred_2(Data data, DSF sf)
      : data_(std::move(data)), sf_(std::move(sf)) {}
  PUSHMI_TEMPLATE(class Out)
    (requires PUSHMI_EXP(defer::Receiver<Out, is_single<>> PUSHMI_AND
        defer::Invocable<DSF&, Data&, Out>))
  void submit(Out out) {
    sf_(data_, std::move(out));
  }
};

template <class A, class B>
using single_deferred_base =
  std::conditional_t<
    (bool)Sender<A, is_single<>>,
    single_deferred_2<A, B>,
    any_single_deferred<A, B>>;
} // namespace detail

template <class A, class B>
struct single_deferred<A, B>
  : detail::single_deferred_base<A, B> {
  constexpr single_deferred() = default;
  using detail::single_deferred_base<A, B>::single_deferred_base;
};

////////////////////////////////////////////////////////////////////////////////
// make_single_deferred
PUSHMI_INLINE_VAR constexpr struct make_single_deferred_fn {
  inline auto operator()() const {
    return single_deferred<ignoreSF>{};
  }
  PUSHMI_TEMPLATE(class SF)
    (requires True<> PUSHMI_BROKEN_SUBSUMPTION(&& not Sender<SF>))
  auto operator()(SF sf) const {
    return single_deferred<SF>{std::move(sf)};
  }
  PUSHMI_TEMPLATE(class Data)
    (requires True<> && Sender<Data, is_single<>>)
  auto operator()(Data d) const {
    return single_deferred<Data, passDSF>{std::move(d)};
  }
  PUSHMI_TEMPLATE(class Data, class DSF)
    (requires Sender<Data, is_single<>>)
  auto operator()(Data d, DSF sf) const {
    return single_deferred<Data, DSF>{std::move(d), std::move(sf)};
  }
} const make_single_deferred {};

////////////////////////////////////////////////////////////////////////////////
// deduction guides
#if __cpp_deduction_guides >= 201703
single_deferred() -> single_deferred<ignoreSF>;

PUSHMI_TEMPLATE(class SF)
  (requires True<> PUSHMI_BROKEN_SUBSUMPTION(&& not Sender<SF>))
single_deferred(SF) -> single_deferred<SF>;

PUSHMI_TEMPLATE(class Data)
  (requires True<> && Sender<Data, is_single<>>)
single_deferred(Data) -> single_deferred<Data, passDSF>;

PUSHMI_TEMPLATE(class Data, class DSF)
  (requires Sender<Data, is_single<>>)
single_deferred(Data, DSF) -> single_deferred<Data, DSF>;
#endif

// template <
//     class V,
//     class E = std::exception_ptr,
//     SenderTo<single<V, E>, is_single<>> Wrapped>
// auto erase_cast(Wrapped w) {
//   return single_deferred<V, E>{std::move(w)};
// }

} // namespace pushmi
