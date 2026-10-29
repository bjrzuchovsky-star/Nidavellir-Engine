// C++ twin of scripts/character_movement.cs. Drives the body itself:
// movement, jumping, gravity, mouse look and grounding. Does not use the
// Rider 3D plugin. Props show in the Inspector via Prop("name", default).
#include <tombstone/script_sdk.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

struct CharacterMovement : ts::Script {
  float moveSpeed = 0;
  float gravity = 0;
  float jumpHeight = 0;
  float mouseSensitivity = 0;
  float upLookLimit = 0;
  float downLookLimit = 0;
  ts::Vector3 velocity = ts::Vector3::Zero();
  bool isGrounded = false;
  float verticalRotation = 0;

  void OnStart() override {
    moveSpeed = Prop("moveSpeed", 6.0f);
    gravity = Prop("gravity", -9.81f);
    jumpHeight = Prop("jumpHeight", 1.5f);
    mouseSensitivity = Prop("mouseSensitivity", 2.0f);
    upLookLimit = Prop("upLookLimit", -80.0f);
    downLookLimit = Prop("downLookLimit", 80.0f);
    Toast("Character Movement Initialized", "Press WASD to move.");
    Log(std::string("Player spawned at initial position: ") + Position().ToString());
  }

  void OnUpdate(float deltaTime) override {
    HandleRotation(deltaTime);
    HandleMovement(deltaTime);
  }

  void on_reload() override { Log("character reloaded y=" + std::to_string(velocity.Y)); }

  std::string save() const override {
    std::ostringstream o;
    o << moveSpeed << ' ' << gravity << ' ' << jumpHeight << ' ' << mouseSensitivity << ' '
      << upLookLimit << ' ' << downLookLimit << ' ' << velocity.X << ' ' << velocity.Y << ' ' << velocity.Z
      << ' ' << (isGrounded ? 1 : 0) << ' ' << verticalRotation;
    return o.str();
  }
  void load(const std::string& s) override {
    std::istringstream in(s);
    int g = 0;
    if (in >> moveSpeed >> gravity >> jumpHeight >> mouseSensitivity >> upLookLimit >> downLookLimit >>
        velocity.X >> velocity.Y >> velocity.Z >> g >> verticalRotation) {
      isGrounded = g != 0;
    }
  }

  void HandleMovement(float deltaTime) {
    isGrounded = Position().Y <= 0.01f;
    if (isGrounded && velocity.Y < 0) velocity.Y = -2.f;
    const float moveX = ts::Input::GetAxis("Horizontal");
    const float moveZ = ts::Input::GetAxis("Vertical");
    const ts::Axes axes = Transform();
    const ts::Vector3 moveDirection = (axes.Right * moveX) + (axes.Forward * moveZ);
    Move(moveDirection * moveSpeed * deltaTime);
    if (ts::Input::GetButtonDown("Jump") && isGrounded) {
      velocity.Y = std::sqrt(jumpHeight * -2.f * gravity);
      sound("player_jump");
    }
    velocity.Y += gravity * deltaTime;
    Move(velocity * deltaTime);
  }

  void HandleRotation(float /*deltaTime*/) {
    const float mouseX = ts::Input::GetAxis("Mouse X") * mouseSensitivity;
    const float mouseY = ts::Input::GetAxis("Mouse Y") * mouseSensitivity;
    Rotate(ts::Vector3::Up() * mouseX);
    verticalRotation -= mouseY;
    verticalRotation = std::clamp(verticalRotation, upLookLimit, downLookLimit);
    if (ts::EntityRef cam = Find("MainCamera")) cam.SetLocalRotation(verticalRotation, 0, 0);
  }
};

TS_SCRIPT(CharacterMovement, "character_movement")
