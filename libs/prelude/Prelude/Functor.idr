module Prelude.Functor

import Prelude.Basics

||| Functors
||| @ f the action of the functor on objects
class Functor (f : Type -> Type) where
    ||| The action of the functor on morphisms
    ||| @ f the functor
    ||| @ m the morphism
    map : (m : a -> b) -> f a -> f b

infixl 4 <$>

||| An infix alias for `map`: the action of a functor on morphisms.
||| @ f the functor
||| @ m the morphism
(<$>) : Functor f => (m : a -> b) -> f a -> f b
m <$> x = map m x

class Functor f => VerifiedFunctor (f : Type -> Type) where
  functorIdentity : {a : Type} -> (x : f a) -> map id x = id x
  functorComposition : {a : Type} -> {b : Type} -> (x : f a) ->
                       (g1 : a -> b) -> (g2 : b -> c) ->
                       map (g2 . g1) x = (map g2 . map g1) x

