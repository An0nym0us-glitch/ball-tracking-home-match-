import 'package:flutter/material.dart';

import 'screens/home_screen.dart';

void main() {
  runApp(const PocketEyeApp());
}

class PocketEyeApp extends StatelessWidget {
  const PocketEyeApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'PocketEye',
      theme: ThemeData(colorSchemeSeed: const Color(0xFF2E7D32), useMaterial3: true),
      darkTheme: ThemeData(colorSchemeSeed: const Color(0xFF2E7D32), brightness: Brightness.dark, useMaterial3: true),
      home: const HomeScreen(),
    );
  }
}
