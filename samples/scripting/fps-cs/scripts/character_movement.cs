// scripts/character_movement.cs -- a TombStone C# script (needs the .NET 8 SDK). Every
// scripts/*.cs builds into one assembly; save and the ride hot-reloads it.
// Props are read with Prop("name", default) and show in the Inspector.
// API: TombStone.Script (Find, Player, Position, Move, SetVisible,
// SetCollider, Destroy, Get / Set, Toast, Log, Sound, Burst...).
using System;
using TombStone;
[Script("character_movement")]
public class CharacterMovement : Script
{
    // Cached setting properties mapped using TombStone's inspector-facing Prop method
    private float moveSpeed;
    private float gravity;
    private float jumpHeight;
    private float mouseSensitivity;
    private float upLookLimit;
    private float downLookLimit;
    private Vector3 velocity = Vector3.Zero;
    private bool isGrounded = false;
    private float verticalRotation = 0.0f;
    // Assuming standard TombStone script life-cycle utilizes OnStart / OnUpdate overrides
    public override void OnStart()
    {
        // Bind settings to the Inspector UI using Prop(name, defaultValue)
        moveSpeed = Prop("moveSpeed", 6.0f);
        gravity = Prop("gravity", -9.81f);
        jumpHeight = Prop("jumpHeight", 1.5f);
        mouseSensitivity = Prop("mouseSensitivity", 2.0f);
        upLookLimit = Prop("upLookLimit", -80.0f);
        downLookLimit = Prop("downLookLimit", 80.0f);
        // Lock and hide the cursor using engine preferences if applicable, 
        // or notify the system on spawn
        Toast("Character Movement Initialized", "Press WASD to move.");
        Log("Player spawned at initial position: " + Position);
    }
    public override void OnUpdate(float deltaTime)
    {
        HandleRotation(deltaTime);
        HandleMovement(deltaTime);
    }
    private void HandleMovement(float deltaTime)
    {
        // 1. Core Grounding check using your environment's raycast/state bounds or layout
        // For physics engines, a minor downward velocity check or native boolean is evaluated.
        // Assuming character is grounded if hitting the zero plane or custom engine flag
        isGrounded = Position.Y <= 0.01f; // Simplified boundary check or replaced with engine state
        if (isGrounded && velocity.Y < 0)
        {
            velocity.Y = -2f; 
        }
        // 2. Fetch input states using TombStone engine-provided axis calls or standard equivalents
        // Replace with system wrapper if input maps are handled natively (e.g., TombStone.Input)
        float moveX = Input.GetAxis("Horizontal"); 
        float moveZ = Input.GetAxis("Vertical");
        // 3. Compute directional movement utilizing the script's local transform axes
        Vector3 moveDirection = (Transform.Right * moveX) + (Transform.Forward * moveZ);
        
        // 4. Translate horizontal movement using the custom API: Move(Vector3 offset)
        Move(moveDirection * moveSpeed * deltaTime);
        // 5. Jump logic mapping tracking the "Jump" trigger state
        if (Input.GetButtonDown("Jump") && isGrounded)
        {
            // Physics formula calculating upward jump velocity
            velocity.Y = (float)Math.Sqrt(jumpHeight * -2f * gravity);
            
            // Optionally play an audio cue utilizing the API
            Sound("player_jump"); 
        }
        // 6. Accumulate gravity forces cleanly over time
        velocity.Y += gravity * deltaTime;
        // 7. Apply resulting vertical velocity via the API
        Move(velocity * deltaTime);
    }
    private void HandleRotation(float deltaTime)
    {
        // Extract look values from target axis tracking mouse look
        float mouseX = Input.GetAxis("Mouse X") * mouseSensitivity;
        float mouseY = Input.GetAxis("Mouse Y") * mouseSensitivity;
        // Horizontally rotate the root object transform around the Y axis
        // In clean custom architectures, this modifies the internal heading orientation directly
        Rotate(Vector3.Up * mouseX);
        // Calculate and constrain look parameters cleanly
        verticalRotation -= mouseY;
        verticalRotation = Math.Clamp(verticalRotation, upLookLimit, downLookLimit);
        // If your look cameras are managed via specific sub-entities, update via Find
        var mainCam = Find("MainCamera");
        if (mainCam != null)
        {
            mainCam.SetLocalRotation(verticalRotation, 0, 0);
        }
    }
}
