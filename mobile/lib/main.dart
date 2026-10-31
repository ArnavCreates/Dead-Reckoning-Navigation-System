import 'dart:async';
import 'package:flutter/material.dart';
import 'models/nav_state.dart';
import 'services/nav_bridge.dart';
import 'widgets/hud_panel.dart';

void main() {
  runApp(const IdrApp());
}

class IdrApp extends StatelessWidget {
  const IdrApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'IDR 26168 Navigation',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        brightness: Brightness.dark,
        scaffoldBackgroundColor: const Color(0xFF0A0F1D),
        colorScheme: const ColorScheme.dark(
          primary: Color(0xFF06B6D4),
          surface: Color(0xFF0F172A),
        ),
      ),
      home: const NavScreen(),
    );
  }
}

class NavScreen extends StatefulWidget {
  const NavScreen({super.key});

  @override
  State<NavScreen> createState() => _NavScreenState();
}

class _NavScreenState extends State<NavScreen> {
  final NavBridge _bridge = NavBridge();
  StreamSubscription<NavState>? _sub;

  NavState _currentState = const NavState(
    tNs: 0,
    lat: 18.9400,
    lon: 72.8450,
    alt: 10.0,
    speedMps: 16.5,
    headingDeg: 24.0,
    mode: NavMode.gnssOk,
    vehicleMode: VehicleClass.car,
    hplM: 3.2,
    ellMajorM: 3.2,
    ellMinorM: 2.1,
    wayId: 1001,
  );

  bool _isNavigating = false;

  @override
  void initState() {
    super.initState();
    _startSession();
  }

  void _startSession() async {
    await _bridge.startNavigation();
    _sub = _bridge.stateStream.listen((state) {
      if (mounted) {
        setState(() {
          _currentState = state;
        });
      }
    });
    setState(() {
      _isNavigating = true;
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    _bridge.stopNavigation();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Stack(
        children: [
          // Simulated Map Background with Corridor Polyline & Grid
          Container(
            color: const Color(0xFF060B14),
            child: CustomPaint(
              size: Size.infinite,
              painter: MapCorridorPainter(
                vehicleHeading: _currentState.headingDeg,
                isDr: _currentState.isDr,
                hplM: _currentState.hplM,
              ),
            ),
          ),

          // Top Header Bar
          SafeArea(
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16.0, vertical: 8.0),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
                    decoration: BoxDecoration(
                      color: const Color(0xFF0F172A).withOpacity(0.9),
                      borderRadius: BorderRadius.circular(12),
                      border: Border.all(color: Colors.white12),
                    ),
                    child: Row(
                      children: const [
                        Icon(Icons.radar, color: Colors.cyanAccent, size: 16),
                        SizedBox(width: 8),
                        Text(
                          "IDR 26168",
                          style: TextStyle(
                            fontWeight: FontWeight.w900,
                            letterSpacing: 1.0,
                            fontSize: 13,
                          ),
                        ),
                      ],
                    ),
                  ),

                  // Outage Indicator Pill
                  if (_currentState.isDr)
                    Container(
                      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
                      decoration: BoxDecoration(
                        color: const Color(0xFFF59E0B).withOpacity(0.2),
                        borderRadius: BorderRadius.circular(12),
                        border: Border.all(color: const Color(0xFFF59E0B)),
                      ),
                      child: Row(
                        children: const [
                          Icon(Icons.gps_off, color: Color(0xFFF59E0B), size: 14),
                          SizedBox(width: 6),
                          Text(
                            "TUNNEL OUTAGE · DR ACTIVE",
                            style: TextStyle(
                              color: Color(0xFFF59E0B),
                              fontWeight: FontWeight.w800,
                              fontSize: 10,
                            ),
                          ),
                        ],
                      ),
                    ),
                ],
              ),
            ),
          ),

          // Bottom Tactical HUD Panel
          Positioned(
            left: 16,
            right: 16,
            bottom: 24,
            child: HudPanel(state: _currentState),
          ),
        ],
      ),
    );
  }
}

class MapCorridorPainter extends CustomPainter {
  final double vehicleHeading;
  final bool isDr;
  final double hplM;

  MapCorridorPainter({
    required this.vehicleHeading,
    required this.isDr,
    required this.hplM,
  });

  @override
  void paint(Canvas canvas, Size size) {
    final cx = size.width / 2;
    final cy = size.height / 2;

    // Draw tactical grid
    final gridPaint = Paint()
      ..color = Colors.white.withOpacity(0.04)
      ..strokeWidth = 1.0;

    for (double x = 0; x < size.width; x += 40) {
      canvas.drawLine(Offset(x, 0), Offset(x, size.height), gridPaint);
    }
    for (double y = 0; y < size.height; y += 40) {
      canvas.drawLine(Offset(0, y), Offset(size.width, y), gridPaint);
    }

    // Road Corridor Polyline
    final roadPaint = Paint()
      ..color = const Color(0xFF0284C7).withOpacity(0.5)
      ..strokeWidth = 16.0
      ..strokeCap = StrokeCap.round
      ..style = PaintingStyle.stroke;

    final roadPath = Path()
      ..moveTo(cx - 30, size.height)
      ..lineTo(cx, cy + 80)
      ..lineTo(cx + 40, cy - 100)
      ..lineTo(cx + 60, 0);

    canvas.drawPath(roadPath, roadPaint);

    // HPL Protection Ellipse Ring
    final hplRadius = (hplM * 6.0).clamp(30.0, 140.0);
    final hplPaint = Paint()
      ..color = isDr
          ? const Color(0xFFF59E0B).withOpacity(0.25)
          : const Color(0xFF06B6D4).withOpacity(0.2)
      ..style = PaintingStyle.fill;

    final hplStroke = Paint()
      ..color = isDr ? const Color(0xFFF59E0B) : const Color(0xFF06B6D4)
      ..strokeWidth = 2.0
      ..style = PaintingStyle.stroke;

    canvas.drawOval(
      Rect.fromCenter(center: Offset(cx, cy + 40), width: hplRadius * 1.6, height: hplRadius),
      hplPaint,
    );
    canvas.drawOval(
      Rect.fromCenter(center: Offset(cx, cy + 40), width: hplRadius * 1.6, height: hplRadius),
      hplStroke,
    );

    // Vehicle Marker Arrow
    final vehiclePaint = Paint()
      ..color = Colors.cyanAccent
      ..style = PaintingStyle.fill;

    final arrowPath = Path()
      ..moveTo(cx, cy + 25)
      ..lineTo(cx - 10, cy + 50)
      ..lineTo(cx, cy + 44)
      ..lineTo(cx + 10, cy + 50)
      ..close();

    canvas.drawPath(arrowPath, vehiclePaint);
  }

  @override
  bool shouldRepaint(covariant MapCorridorPainter oldDelegate) {
    return oldDelegate.vehicleHeading != vehicleHeading ||
        oldDelegate.isDr != isDr ||
        oldDelegate.hplM != hplM;
  }
}
