/*
 * Copyright 2021-2022 Brennan Shacklett and contributors
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */
#pragma once

#include <cstdint>
#include <cmath>
#include <cfloat>
#include <cstdio>

#include <madrona/types.hpp>

namespace madrona::math {

constexpr inline float pi {3.14159265358979323846264338327950288f};
constexpr inline float pi_d2 {pi / 2.f};
constexpr inline float pi_m2 {pi * 2.f};

inline constexpr float toRadians(float degrees);

struct Vector2 {
    float x;
    float y;

    inline float dot(const Vector2 &o) const;

    inline float length2() const;
    inline float length() const;
    inline float invLength() const;

    inline float & operator[](CountT i);
    inline float operator[](CountT i) const;

    inline Vector2 & operator+=(const Vector2 &o);
    inline Vector2 & operator-=(const Vector2 &o);

    inline Vector2 & operator+=(float o);
    inline Vector2 & operator-=(float o);
    inline Vector2 & operator*=(float o);
    inline Vector2 & operator/=(float o);

    friend inline Vector2 operator+(Vector2 v);
    friend inline Vector2 operator-(Vector2 v);

    friend inline Vector2 operator+(Vector2 a, const Vector2 &b);
    friend inline Vector2 operator-(Vector2 a, const Vector2 &b);

    friend inline Vector2 operator+(Vector2 a, float b);
    friend inline Vector2 operator-(Vector2 a, float b);
    friend inline Vector2 operator*(Vector2 a, float b);
    friend inline Vector2 operator/(Vector2 a, float b);

    friend inline Vector2 operator+(float a, Vector2 b);
    friend inline Vector2 operator-(float a, Vector2 b);
    friend inline Vector2 operator*(float a, Vector2 b);
    friend inline Vector2 operator/(float a, Vector2 b);
};

struct Vector3 {
    float x;
    float y;
    float z;

    inline float dot(const Vector3 &o) const;
    inline Vector3 cross(const Vector3 &o) const;

    // Returns two vectors perpendicular to this vector
    // *this should be a normalized
    inline void frame(Vector3 *a, Vector3 *b) const;

    inline float length2() const;
    inline float length() const;
    inline float invLength() const;

    inline float distance(const Vector3 &o) const;
    inline float distance2(const Vector3 &o) const;

    [[nodiscard]] inline Vector3 normalize() const;

    inline float & operator[](CountT i);
    inline float operator[](CountT i) const;

    inline Vector3 & operator+=(const Vector3 &o);
    inline Vector3 & operator-=(const Vector3 &o);

    inline Vector3 & operator+=(float o);
    inline Vector3 & operator-=(float o);
    inline Vector3 & operator*=(float o);
    inline Vector3 & operator/=(float o);

    friend inline Vector3 operator-(Vector3 v);

    friend inline Vector3 operator+(Vector3 a, const Vector3 &b);
    friend inline Vector3 operator-(Vector3 a, const Vector3 &b);

    friend inline Vector3 operator+(Vector3 a, float b);
    friend inline Vector3 operator-(Vector3 a, float b);
    friend inline Vector3 operator*(Vector3 a, float b);
    friend inline Vector3 operator/(Vector3 a, float b);

    friend inline Vector3 operator+(float a, Vector3 b);
    friend inline Vector3 operator-(float a, Vector3 b);
    friend inline Vector3 operator*(float a, Vector3 b);
    friend inline Vector3 operator/(float a, Vector3 b);

    static inline Vector3 min(Vector3 a, Vector3 b);
    static inline Vector3 max(Vector3 a, Vector3 b);

    static constexpr inline Vector3 zero();
    static constexpr inline Vector3 one();
};

inline float dot(Vector3 a, Vector3 b);
inline Vector3 cross(Vector3 a, Vector3 b);
inline Vector3 normalize(Vector3 v);

struct Vector4 {
    float x;
    float y;
    float z;
    float w;

    inline Vector3 xyz() const;

    inline float & operator[](CountT i);
    inline float operator[](CountT i) const;

    static inline Vector4 fromVector3(Vector3 v, float w);

    static constexpr inline Vector4 zero();
    static constexpr inline Vector4 one();
};

struct Quat {
    float w;
    float x;
    float y;
    float z;

    inline float length2() const;
    inline float length() const;
    inline float invLength() const;

    [[nodiscard]] inline Quat normalize() const;
    [[nodiscard]] inline Quat inv() const;

    inline Vector3 rotateVec(Vector3 v) const;

    static inline Quat angleAxis(float angle, Vector3 normal);
    static inline Quat fromAngularVec(Vector3 v);
    static inline Quat fromBasis(Vector3 a, Vector3 b, Vector3 c);

    inline Quat & operator+=(Quat o);
    inline Quat & operator-=(Quat o);
    inline Quat & operator*=(Quat o);
    inline Quat & operator*=(float f);

    friend inline Quat operator+(Quat a, Quat b);
    friend inline Quat operator-(Quat a, Quat b);
    friend inline Quat operator*(Quat a, Quat b);
    friend inline Quat operator*(Quat a, float b);
    friend inline Quat operator*(float b, Quat a);
};

struct Diag3x3 {
    float d0;
    float d1;
    float d2;

    inline Diag3x3 inv() const;

    static inline Diag3x3 fromVec(Vector3 v);

    static constexpr inline Diag3x3 uniform(float scale = 1.f);

    inline Diag3x3 & operator*=(Diag3x3 o);
    inline Diag3x3 & operator*=(float o);

    friend inline Diag3x3 operator*(Diag3x3 a, Diag3x3 b);
    friend inline Diag3x3 operator*(Diag3x3 a, float b);
    friend inline Diag3x3 operator*(float a, Diag3x3 b);
    friend inline Vector3 operator*(Diag3x3 d, Vector3 v);
};

struct Mat3x3 {
    Vector3 cols[3];

    static inline Mat3x3 fromQuat(Quat r);
    static inline Mat3x3 fromRS(Quat r, Diag3x3 s);

    inline Vector3 & operator[](CountT i);
    inline Vector3 operator[](CountT i) const;

    inline Vector3 operator*(Vector3 v);
    inline Mat3x3 operator*(const Mat3x3 &o);
    inline Mat3x3 & operator*=(const Mat3x3 &o);

    friend inline Mat3x3 operator*(const Mat3x3 &m, Diag3x3 d);
    friend inline Mat3x3 operator*(Diag3x3 d, const Mat3x3 &m);
};

struct Mat3x4 {
    Vector3 cols[4];

    inline Vector3 txfmPoint(Vector3 p) const;
    inline Vector3 txfmDir(Vector3 p) const;

    inline Mat3x4 compose(const Mat3x4 &o) const;

    inline void decompose(Vector3 *translation,
                          Quat *rotation,
                          Diag3x3 *scale) const;

    static inline Mat3x4 fromRows(Vector4 row0, Vector4 row1, Vector4 row2);
    static inline Mat3x4 fromTRS(Vector3 t, Quat r,
                                 Diag3x3 s = { 1.f, 1.f, 1.f });

    static constexpr inline Mat3x4 identity();
};

struct Mat4x4 {
    Vector4 cols[4];
};

struct AABB {
    Vector3 pMin;
    Vector3 pMax;

    inline float surfaceArea() const;
    inline bool overlaps(const AABB &o) const;
    inline bool contains(const AABB &o) const;
    inline void expand(const Vector3 &p);

    inline bool rayIntersects(Vector3 ray_o, Diag3x3 inv_ray_d,
                              float ray_t_min, float ray_t_max);

    [[nodiscard]] inline AABB applyTRS(const Vector3 &translation,
                                       const Quat &rotation,
                                       const Diag3x3 &scale = {1, 1, 1}) const;

    static inline AABB invalid();
    static inline AABB point(const Vector3 &p);
    static inline AABB merge(const AABB &a, const AABB &b);
};

constexpr inline Vector3 up { 0, 0, 1 };
constexpr inline Vector3 fwd { 0, 1, 0 };
constexpr inline Vector3 right { 1, 0, 0 };

}

#include "math.inl"
