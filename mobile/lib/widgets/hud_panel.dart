import 'package:flutter/material.dart';
import '../models/nav_state.dart';

class HudPanel extends StatelessWidget {
  final NavState state;

  const HudPanel({super.key, required this.state});

  @override
  Widget build(BuildContext context) {
    Color modeColor = const Color(0xFF10B981); // Emerald
    if (state.mode == NavMode.dr) {
      modeColor = const Color(0xFFF59E0B); // Amber
    } else if (state.mode == NavMode.degraded) {
      modeColor = const Color(0xFFF97316); // Orange
    } else if (state.mode == NavMode.suspectRf) {
      modeColor = const Color(0xFFF43F5E); // Rose
    } else if (state.mode == NavMode.reacq) {
      modeColor = const Color(0xFF06B6D4); // Cyan
    }

    final alertLimitM = 20.0;
    final hplRatio = (state.hplM / alertLimitM).clamp(0.0, 1.0);

    return Container(
      padding: const EdgeInsets.all(16.0),
      decoration: BoxDecoration(
        color: const Color(0xFF1E293B).withOpacity(0.85),
        borderRadius: BorderRadius.circular(20.0),
        border: Border.all(color: Colors.white.withOpacity(0.1)),
        boxShadow: [
          BoxShadow(
            color: Colors.black.withOpacity(0.4),
            blurRadius: 20,
            offset: const Offset(0, 10),
          ),
        ],
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          // Top Row: Mode Badge & Vehicle Type
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                decoration: BoxDecoration(
                  color: modeColor.withOpacity(0.15),
                  border: Border.all(color: modeColor.withOpacity(0.5)),
                  borderRadius: BorderRadius.circular(8),
                ),
                child: Text(
                  state.modeString,
                  style: TextStyle(
                    color: modeColor,
                    fontSize: 11,
                    fontWeight: FontWeight.w800,
                    letterSpacing: 0.5,
                  ),
                ),
              ),
              Row(
                children: [
                  Icon(
                    state.vehicleMode == VehicleClass.car ? Icons.directions_car : Icons.two_wheeler,
                    color: Colors.cyanAccent,
                    size: 16,
                  ),
                  const SizedBox(width: 4),
                  Text(
                    state.vehicleMode == VehicleClass.car ? "CAR" : "2-WHEELER",
                    style: const TextStyle(
                      color: Colors.white70,
                      fontSize: 11,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                ],
              ),
            ],
          ),

          const SizedBox(height: 12),

          // Middle: Big Speedometer & Heading
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            crossAxisAlignment: CrossAxisAlignment.baseline,
            textBaseline: TextBaseline.alphabetic,
            children: [
              Row(
                crossAxisAlignment: CrossAxisAlignment.baseline,
                textBaseline: TextBaseline.alphabetic,
                children: [
                  Text(
                    state.speedKmh.toStringAsFixed(0),
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 48,
                      fontWeight: FontWeight.w900,
                      fontFamily: "monospace",
                    ),
                  ),
                  const SizedBox(width: 6),
                  const Text(
                    "KM/H",
                    style: TextStyle(
                      color: Colors.white54,
                      fontSize: 14,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                ],
              ),
              Column(
                crossAxisAlignment: CrossAxisAlignment.end,
                children: [
                  Text(
                    "${state.headingDeg.toStringAsFixed(0)}°",
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 18,
                      fontWeight: FontWeight.bold,
                      fontFamily: "monospace",
                    ),
                  ),
                  const Text(
                    "HEADING",
                    style: TextStyle(color: Colors.white38, fontSize: 10),
                  ),
                ],
              ),
            ],
          ),

          const SizedBox(height: 12),

          // HPL vs Alert Limit Progress Bar
          Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  const Text(
                    "PROTECTION LEVEL (HPL)",
                    style: TextStyle(
                      color: Colors.white60,
                      fontSize: 10,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  Text(
                    "${state.hplM.toStringAsFixed(1)} m / 20.0 m",
                    style: const TextStyle(
                      color: Colors.cyanAccent,
                      fontSize: 11,
                      fontWeight: FontWeight.bold,
                      fontFamily: "monospace",
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 6),
              ClipRRect(
                borderRadius: BorderRadius.circular(4),
                child: LinearProgressIndicator(
                  value: hplRatio,
                  minHeight: 6,
                  backgroundColor: Colors.white10,
                  valueColor: AlwaysStoppedAnimation<Color>(
                    state.hplM > alertLimitM
                        ? const Color(0xFFF43F5E)
                        : (state.hplM > 12 ? const Color(0xFFF59E0B) : const Color(0xFF06B6D4)),
                  ),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}
